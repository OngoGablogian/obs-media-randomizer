/*
 * Random Clip Player - OBS source
 *
 * Plays a random video file from a folder when a hotkey is pressed.
 * Each clip is loaded into a temporary private "ffmpeg_source" (the same
 * decoder OBS's own Media Source uses). As soon as the clip finishes, or is
 * stopped, that child source is destroyed, so no file handles, decoder
 * threads or frame buffers are held while idle.
 *
 * Optional modes:
 *  - Loop: keep playing random clips (shuffled, every clip once per round)
 *    until stopped, hidden, or the scene changes.
 *  - Auto-start: start playing when the program scene switches to a scene
 *    that shows this source.
 *  - Stop on scene change (default on): stop the clip on any scene switch,
 *    even if the new scene also contains this source.
 *
 * Threading model:
 *  - Hotkeys/buttons/frontend events only set atomic flags.
 *  - The folder scan and media source creation run on a background worker,
 *    so disk access never stalls OBS's render thread.
 *  - video_tick (graphics thread) installs the loaded clip and unloads it
 *    when it ends. OBS itself destroys released sources on a background
 *    thread.
 *  - While idle, tick does a few atomic reads and render returns after one.
 *  - Only one clip is ever loaded at a time, including in loop mode.
 *  - The clip's audio is relayed through this source via an audio capture
 *    callback.
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/task.h>

#include <stdlib.h>
#include <string.h>

#include "random-clip-source.h"

#define S_FOLDER "folder"
#define S_RECURSIVE "recursive"
#define S_REPEAT_MODE "repeat_mode"
#define S_INTERRUPT "interrupt"
#define S_LOOP "loop"
#define S_AUTOSTART "autostart"
#define S_STOP_ON_SCENE "stop_on_scene_change"
#define S_STOP_ON_HIDE "stop_on_hide"
#define S_FADE "fade_ms"
#define S_FADE_LOOP "fade_loop"
#define S_FADE_SINGLE "fade_single"
#define S_HW_DECODE "hw_decode"
#define S_WIDTH "width"
#define S_HEIGHT "height"

#define T_(s) obs_module_text(s)

#define SOURCE_ID "random_clip_player"

/* repeat_mode values */
#define REPEAT_ALLOW 0     /* fully random, repeats allowed */
#define REPEAT_NOT_TWICE 1 /* never the same clip twice in a row */
#define REPEAT_SHUFFLE 2   /* every clip once before any repeats */
#define MAX_SCAN_DEPTH 8
#define MIN_FADE_MS 50
#define MAX_FADE_MS 5000
#define GAIN_SCALE 1000                        /* audio gain stored as an integer, 0..GAIN_SCALE */
#define MAX_LOOP_FAILURES 3                    /* end a loop after this many unplayable clips in a row */
#define START_TIMEOUT_NS (10ULL * 1000000000ULL) /* give up if a clip never starts */
#define MIN_CLIP_AGE_NS (500ULL * 1000000ULL)    /* ignore state checks right after start */

static const char *video_exts[] = {".mp4", ".mkv", ".mov",  ".webm", ".avi",  ".flv", ".m4v",
				   ".ts",  ".mts", ".m2ts", ".mpg",  ".mpeg", ".wmv", ".gif"};

/* Bumped by the frontend whenever the program scene changes. */
static volatile long scene_gen = 0;

struct file_list {
	char **items;
	size_t num;
	size_t cap;
};

struct random_clip {
	obs_source_t *source;

	pthread_mutex_t mutex;
	obs_source_t *child;         /* protected by mutex */
	obs_source_t *pending_child; /* loaded by worker, protected by mutex */
	char *pending_path;          /* protected by mutex */
	char *last_file;             /* protected by mutex */
	struct file_list played;     /* clips played this loop round, protected by mutex */
	char *folder;                /* protected by mutex */
	bool recursive;
	int repeat_mode;
	bool interrupt;
	bool loop;
	bool autostart;
	bool stop_on_scene;
	bool stop_on_hide;
	bool hw_decode;
	uint32_t cx;
	uint32_t cy;
	int fade_ms;
	bool fade_loop;
	bool fade_single;

	/* graphics-thread only */
	uint64_t clip_start_ns;
	bool clip_played; /* current clip reached the playing state */
	bool clip_fades;  /* current clip fades in/out (started by a loop) */
	float opacity;    /* current fade level, 0..1 */

	/* fade rendering, graphics thread only; the texture only exists while
	 * a clip is loaded and has actually needed a fade frame */
	gs_effect_t *fade_effect;
	gs_eparam_t *fade_image;
	gs_eparam_t *fade_opacity;
	gs_texrender_t *texrender;
	bool fade_effect_failed;

	/* audio fade: target gain set by tick, applied by the audio callback */
	volatile long gain;
	float last_gain;     /* audio callback thread only */
	float *gain_buf;     /* audio callback thread only */
	size_t gain_buf_len; /* in floats */
	bool looping;        /* a loop session is running */
	int loop_failures;
	long seen_scene_gen;

	/* random number state, only used by this source's worker thread */
	uint64_t rng;

	/* set by graphics thread before queueing a load, read by the worker */
	long load_gen;
	bool load_shuffle;

	os_task_queue_t *loader;
	long gen; /* protected by mutex; bumped on stop to cancel in-flight loads */
	volatile bool loading;
	volatile bool pending_ready;
	volatile bool load_empty; /* last load found nothing to play */
	volatile bool has_clip;   /* fast idle check for tick/render */

	/* OBS audio format, read once at creation */
	enum speaker_layout speakers;
	uint32_t samples_per_sec;

	volatile bool play_requested;
	volatile bool stop_requested;
	volatile bool autostart_requested;

	obs_hotkey_id play_hotkey;
	obs_hotkey_id stop_hotkey;
};

/* ------------------------------------------------------------------------- */
/* File scanning                                                              */

static void file_list_push(struct file_list *list, char *path)
{
	if (list->num == list->cap) {
		list->cap = list->cap ? list->cap * 2 : 32;
		list->items = brealloc(list->items, list->cap * sizeof(char *));
	}
	list->items[list->num++] = path;
}

static void file_list_free(struct file_list *list)
{
	for (size_t i = 0; i < list->num; i++)
		bfree(list->items[i]);
	bfree(list->items);
	memset(list, 0, sizeof(*list));
}

static bool file_list_contains(const struct file_list *list, const char *path)
{
	for (size_t i = 0; i < list->num; i++) {
		if (strcmp(list->items[i], path) == 0)
			return true;
	}
	return false;
}

static bool is_video_file(const char *name)
{
	const char *ext = os_get_path_extension(name);
	if (!ext)
		return false;
	for (size_t i = 0; i < sizeof(video_exts) / sizeof(video_exts[0]); i++) {
		if (astrcmpi(ext, video_exts[i]) == 0)
			return true;
	}
	return false;
}

static void scan_folder(const char *dir_path, bool recursive, int depth, struct file_list *out)
{
	os_dir_t *dir = os_opendir(dir_path);
	if (!dir)
		return;

	struct os_dirent *ent;
	while ((ent = os_readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;

		struct dstr path = {0};
		dstr_printf(&path, "%s/%s", dir_path, ent->d_name);

		if (ent->directory) {
			if (recursive && depth < MAX_SCAN_DEPTH)
				scan_folder(path.array, recursive, depth + 1, out);
			dstr_free(&path);
		} else if (is_video_file(ent->d_name)) {
			file_list_push(out, path.array); /* takes ownership */
		} else {
			dstr_free(&path);
		}
	}

	os_closedir(dir);
}

/* SplitMix64: a tiny, fast, high-quality random number generator. Each
 * source has its own state, seeded once from the clock and its address,
 * so every source and every OBS session gets a different sequence. */
static uint64_t rng_next(uint64_t *state)
{
	uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

/* Uniform random number in [0, n) with no modulo bias. */
static size_t rng_below(uint64_t *state, size_t n)
{
	uint64_t limit = UINT64_MAX - (UINT64_MAX % (uint64_t)n);
	uint64_t r;
	do {
		r = rng_next(state);
	} while (r >= limit);
	return (size_t)(r % (uint64_t)n);
}

static inline bool is_last(const char *path, const char *last)
{
	return last && strcmp(path, last) == 0;
}

/* Returns a newly allocated path, or NULL if the folder has no clips.
 * shuffle: pick only clips not yet played this round. */
static char *pick_random_file(struct random_clip *rc, bool shuffle)
{
	pthread_mutex_lock(&rc->mutex);
	char *folder = rc->folder ? bstrdup(rc->folder) : NULL;
	bool recursive = rc->recursive;
	bool no_repeat = rc->repeat_mode != REPEAT_ALLOW;
	char *last = rc->last_file ? bstrdup(rc->last_file) : NULL;
	pthread_mutex_unlock(&rc->mutex);

	if (!folder || !*folder) {
		blog(LOG_WARNING, "[Random Clip Player] No folder selected");
		bfree(folder);
		bfree(last);
		return NULL;
	}

	struct file_list files = {0};
	scan_folder(folder, recursive, 0, &files);

	char *result = NULL;
	if (files.num == 0) {
		blog(LOG_WARNING, "[Random Clip Player] No video files found in '%s'", folder);
	} else {
		size_t *cand = bmalloc(files.num * sizeof(size_t));
		size_t n = 0;

		if (shuffle) {
			pthread_mutex_lock(&rc->mutex);
			for (size_t i = 0; i < files.num; i++) {
				if (!file_list_contains(&rc->played, files.items[i]))
					cand[n++] = i;
			}
			if (n == 0) {
				/* every clip has played: start a new round, but never
				 * repeat the clip that just finished */
				file_list_free(&rc->played);
				for (size_t i = 0; i < files.num; i++) {
					if (files.num == 1 || !is_last(files.items[i], last))
						cand[n++] = i;
				}
			}
			pthread_mutex_unlock(&rc->mutex);
		} else {
			for (size_t i = 0; i < files.num; i++) {
				if (!no_repeat || files.num == 1 || !is_last(files.items[i], last))
					cand[n++] = i;
			}
		}

		if (n == 0) {
			for (size_t i = 0; i < files.num; i++)
				cand[n++] = i;
		}

		size_t idx = cand[rng_below(&rc->rng, n)];
		result = files.items[idx];
		files.items[idx] = NULL;
		bfree(cand);

		if (shuffle) {
			pthread_mutex_lock(&rc->mutex);
			file_list_push(&rc->played, bstrdup(result));
			pthread_mutex_unlock(&rc->mutex);
		}
	}

	file_list_free(&files);
	bfree(folder);
	bfree(last);
	return result;
}

/* ------------------------------------------------------------------------- */
/* Child (clip) management                                                   */

static obs_source_t *get_child_ref(struct random_clip *rc)
{
	pthread_mutex_lock(&rc->mutex);
	obs_source_t *child = rc->child ? obs_source_get_ref(rc->child) : NULL;
	pthread_mutex_unlock(&rc->mutex);
	return child;
}

/* Relays the clip's audio (already converted to OBS's mix format) out of this
 * source, so it gets this source's mixer fader, filters and monitoring. */
static void forward_audio(void *param, obs_source_t *child, const struct audio_data *audio, bool muted)
{
	UNUSED_PARAMETER(child);
	UNUSED_PARAMETER(muted);
	struct random_clip *rc = param;

	struct obs_source_audio out = {0};
	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		out.data[i] = audio->data[i];
	out.frames = audio->frames;
	out.speakers = rc->speakers;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = rc->samples_per_sec;
	out.timestamp = audio->timestamp;

	/* Fading: scale a copy of the samples, ramping from the previous gain
	 * to the new one across the packet so the volume changes smoothly.
	 * At full volume the samples are passed through untouched. */
	float gain = (float)os_atomic_load_long(&rc->gain) / (float)GAIN_SCALE;
	float start = rc->last_gain;
	rc->last_gain = gain;

	if ((gain < 1.0f || start < 1.0f) && audio->frames) {
		size_t channels = get_audio_channels(rc->speakers);
		size_t frames = audio->frames;
		size_t needed = channels * frames;

		if (rc->gain_buf_len < needed) {
			rc->gain_buf = brealloc(rc->gain_buf, needed * sizeof(float));
			rc->gain_buf_len = needed;
		}

		float step = (gain - start) / (float)frames;
		for (size_t ch = 0; ch < channels && ch < MAX_AV_PLANES; ch++) {
			const float *in = (const float *)audio->data[ch];
			float *dst = rc->gain_buf + ch * frames;
			if (!in)
				continue;
			float g = start;
			for (size_t i = 0; i < frames; i++) {
				dst[i] = in[i] * g;
				g += step;
			}
			out.data[ch] = (const uint8_t *)dst;
		}
	}

	obs_source_output_audio(rc->source, &out);
}

static void stop_clip(struct random_clip *rc)
{
	pthread_mutex_lock(&rc->mutex);
	obs_source_t *child = rc->child;
	rc->child = NULL;
	pthread_mutex_unlock(&rc->mutex);
	os_atomic_set_bool(&rc->has_clip, false);

	if (child) {
		obs_source_remove_audio_capture_callback(child, forward_audio, rc);
		obs_source_remove_active_child(rc->source, child);
		obs_source_release(child); /* destroys the decoder and closes the file */
	}
}

/* Background worker: scan the folder, pick a clip and create its decoder. */
static void load_task(void *param)
{
	struct random_clip *rc = param;
	long gen = rc->load_gen;
	bool shuffle = rc->load_shuffle;

	char *path = pick_random_file(rc, shuffle);
	obs_source_t *child = NULL;

	if (path) {
		pthread_mutex_lock(&rc->mutex);
		bool hw_decode = rc->hw_decode;
		pthread_mutex_unlock(&rc->mutex);

		obs_data_t *settings = obs_data_create();
		obs_data_set_bool(settings, "is_local_file", true);
		obs_data_set_string(settings, "local_file", path);
		obs_data_set_bool(settings, "looping", false);
		obs_data_set_bool(settings, "restart_on_activate", false); /* start immediately */
		obs_data_set_bool(settings, "close_when_inactive", false);
		obs_data_set_bool(settings, "clear_on_media_end", true);
		obs_data_set_bool(settings, "full_decode", false); /* stream from disk, don't preload */
		obs_data_set_bool(settings, "hw_decode", hw_decode);
		obs_data_set_int(settings, "speed_percent", 100);

		child = obs_source_create_private("ffmpeg_source", "Random Clip Player (clip)", settings);
		obs_data_release(settings);

		if (!child)
			blog(LOG_ERROR, "[Random Clip Player] Failed to create media source for '%s'", path);
	}

	if (child) {
		/* hand off, unless a stop/hide happened while loading */
		pthread_mutex_lock(&rc->mutex);
		bool cancelled = gen != rc->gen;
		if (!cancelled) {
			rc->pending_child = child;
			rc->pending_path = path;
		}
		pthread_mutex_unlock(&rc->mutex);

		if (cancelled) {
			obs_source_release(child);
			bfree(path);
		} else {
			os_atomic_set_bool(&rc->pending_ready, true);
		}
	} else {
		bfree(path);
		os_atomic_set_bool(&rc->load_empty, true);
	}

	os_atomic_set_bool(&rc->loading, false);
}

/* Cancels any in-flight load and drops a loaded-but-not-yet-shown clip. */
static void cancel_loads(struct random_clip *rc)
{
	pthread_mutex_lock(&rc->mutex);
	rc->gen++;
	obs_source_t *child = rc->pending_child;
	char *path = rc->pending_path;
	rc->pending_child = NULL;
	rc->pending_path = NULL;
	pthread_mutex_unlock(&rc->mutex);
	os_atomic_set_bool(&rc->pending_ready, false);

	obs_source_release(child);
	bfree(path);
}

/* Graphics thread: swap in the clip the worker loaded. */
static void install_pending(struct random_clip *rc)
{
	pthread_mutex_lock(&rc->mutex);
	obs_source_t *child = rc->pending_child;
	char *path = rc->pending_path;
	rc->pending_child = NULL;
	rc->pending_path = NULL;
	pthread_mutex_unlock(&rc->mutex);

	if (!child) {
		bfree(path);
		return;
	}

	stop_clip(rc);

	blog(LOG_INFO, "[Random Clip Player] Playing '%s'", path);

	pthread_mutex_lock(&rc->mutex);
	bfree(rc->last_file);
	rc->last_file = path;
	rc->child = child;
	pthread_mutex_unlock(&rc->mutex);

	rc->clip_start_ns = os_gettime_ns();
	rc->clip_played = false;

	pthread_mutex_lock(&rc->mutex);
	rc->clip_fades = (rc->looping ? rc->fade_loop : rc->fade_single) && rc->fade_ms > 0;
	pthread_mutex_unlock(&rc->mutex);
	rc->opacity = rc->clip_fades ? 0.0f : 1.0f;
	rc->last_gain = rc->opacity; /* callback not registered yet, safe to set */
	os_atomic_set_long(&rc->gain, rc->clip_fades ? 0 : GAIN_SCALE);

	obs_source_add_audio_capture_callback(child, forward_audio, rc);
	obs_source_add_active_child(rc->source, child);
	os_atomic_set_bool(&rc->has_clip, true);
}

static bool clip_finished(struct random_clip *rc, obs_source_t *child)
{
	uint64_t age = os_gettime_ns() - rc->clip_start_ns;

	switch (obs_source_media_get_state(child)) {
	case OBS_MEDIA_STATE_PLAYING:
	case OBS_MEDIA_STATE_PAUSED:
		rc->clip_played = true;
		return false;
	case OBS_MEDIA_STATE_ENDED:
	case OBS_MEDIA_STATE_STOPPED:
	case OBS_MEDIA_STATE_ERROR:
		return age >= MIN_CLIP_AGE_NS;
	default:
		/* never managed to start (bad/unreadable file) */
		return age > START_TIMEOUT_NS;
	}
}

/* Graphics thread: fade level for the current clip, from its own playback
 * position, so the fade-out finishes exactly when the clip ends. */
static float compute_opacity(struct random_clip *rc, obs_source_t *child)
{
	if (!rc->clip_fades)
		return 1.0f;

	pthread_mutex_lock(&rc->mutex);
	int fade_ms = rc->fade_ms;
	pthread_mutex_unlock(&rc->mutex);
	if (fade_ms <= 0)
		return 1.0f;

	int64_t t = obs_source_media_get_time(child);
	int64_t dur = obs_source_media_get_duration(child);
	if (t < 0)
		t = 0;

	float level = (float)t / (float)fade_ms;
	if (dur > 0) {
		float out = (float)(dur - t) / (float)fade_ms;
		if (out < level)
			level = out;
	}

	if (level < 0.0f)
		level = 0.0f;
	if (level > 1.0f)
		level = 1.0f;
	return level;
}

/* Graphics thread: free the fade texture once no clip needs it. */
static void free_fade_texture(struct random_clip *rc)
{
	if (!rc->texrender)
		return;
	obs_enter_graphics();
	gs_texrender_destroy(rc->texrender);
	obs_leave_graphics();
	rc->texrender = NULL;
}

/* Graphics thread: queue a background load unless one is already running. */
static void queue_load(struct random_clip *rc)
{
	if (!rc->loader || os_atomic_set_bool(&rc->loading, true))
		return;

	pthread_mutex_lock(&rc->mutex);
	rc->load_gen = rc->gen;
	rc->load_shuffle = rc->looping || rc->repeat_mode == REPEAT_SHUFFLE;
	pthread_mutex_unlock(&rc->mutex);

	if (!os_task_queue_queue_task(rc->loader, load_task, rc))
		os_atomic_set_bool(&rc->loading, false);
}

/* Graphics thread: handle a play request (hotkey, button or auto-start).
 * Auto-start never interrupts a clip that is already playing. */
static void start_playing(struct random_clip *rc, bool from_autostart)
{
	/* Never start a clip nobody can see. Otherwise a hotkey shared by
	 * several sources starts clips in scenes that aren't on screen, and
	 * those clips appear mid-transition when switching to that scene. */
	if (!obs_source_showing(rc->source)) {
		blog(LOG_INFO, "[Random Clip Player] '%s' is not visible, ignoring play request",
		     obs_source_get_name(rc->source));
		return;
	}

	pthread_mutex_lock(&rc->mutex);
	bool interrupt = rc->interrupt && !from_autostart;
	bool loop = rc->loop;
	if (loop && !rc->looping && rc->repeat_mode != REPEAT_SHUFFLE)
		file_list_free(&rc->played); /* new loop session: fresh round */
	pthread_mutex_unlock(&rc->mutex);

	if (loop && !rc->looping) {
		rc->looping = true;
		rc->loop_failures = 0;
	}

	if (!os_atomic_load_bool(&rc->has_clip) || interrupt)
		queue_load(rc);
}

static void stop_playing(struct random_clip *rc)
{
	rc->looping = false;
	cancel_loads(rc);
	stop_clip(rc);
}

/* ------------------------------------------------------------------------- */
/* Frontend hooks (called from plugin-main.c on the UI thread)                */

void random_clip_scene_changed(void)
{
	os_atomic_inc_long(&scene_gen);
}

void random_clip_mark_autostart(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	UNUSED_PARAMETER(param);

	const char *id = obs_source_get_unversioned_id(child);
	if (!id || strcmp(id, SOURCE_ID) != 0)
		return;

	struct random_clip *rc = obs_obj_get_data(child);
	if (rc)
		os_atomic_set_bool(&rc->autostart_requested, true);
}

/* ------------------------------------------------------------------------- */
/* Hotkeys and buttons                                                        */

static void play_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct random_clip *rc = data;
	if (pressed)
		os_atomic_set_bool(&rc->play_requested, true);
}

static void stop_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct random_clip *rc = data;
	if (pressed)
		os_atomic_set_bool(&rc->stop_requested, true);
}

static bool play_button_cb(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct random_clip *rc = data;
	if (rc)
		os_atomic_set_bool(&rc->play_requested, true);
	return false;
}

static bool stop_button_cb(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct random_clip *rc = data;
	if (rc)
		os_atomic_set_bool(&rc->stop_requested, true);
	return false;
}

/* ------------------------------------------------------------------------- */
/* obs_source_info callbacks                                                  */

static const char *rc_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("RandomClipPlayer");
}

static void rc_update(void *data, obs_data_t *settings)
{
	struct random_clip *rc = data;

	const char *folder = obs_data_get_string(settings, S_FOLDER);
	bool recursive = obs_data_get_bool(settings, S_RECURSIVE);

	pthread_mutex_lock(&rc->mutex);
	if (!rc->folder || strcmp(rc->folder, folder) != 0 || rc->recursive != recursive)
		file_list_free(&rc->played); /* different clips: start a new round */
	bfree(rc->folder);
	rc->folder = bstrdup(folder);
	rc->recursive = recursive;
	rc->repeat_mode = (int)obs_data_get_int(settings, S_REPEAT_MODE);
	rc->interrupt = obs_data_get_bool(settings, S_INTERRUPT);
	rc->loop = obs_data_get_bool(settings, S_LOOP);
	rc->autostart = obs_data_get_bool(settings, S_AUTOSTART);
	rc->stop_on_scene = obs_data_get_bool(settings, S_STOP_ON_SCENE);
	rc->stop_on_hide = obs_data_get_bool(settings, S_STOP_ON_HIDE);
	rc->fade_ms = (int)obs_data_get_int(settings, S_FADE);
	rc->fade_loop = obs_data_get_bool(settings, S_FADE_LOOP);
	rc->fade_single = obs_data_get_bool(settings, S_FADE_SINGLE);
	rc->hw_decode = obs_data_get_bool(settings, S_HW_DECODE);
	rc->cx = (uint32_t)obs_data_get_int(settings, S_WIDTH);
	rc->cy = (uint32_t)obs_data_get_int(settings, S_HEIGHT);
	pthread_mutex_unlock(&rc->mutex);
}

static void *rc_create(obs_data_t *settings, obs_source_t *source)
{
	struct random_clip *rc = bzalloc(sizeof(*rc));
	rc->source = source;
	rc->seen_scene_gen = os_atomic_load_long(&scene_gen);
	rc->opacity = 1.0f;
	rc->last_gain = 1.0f;
	rc->gain = GAIN_SCALE;
	rc->rng = os_gettime_ns() ^ ((uint64_t)(uintptr_t)rc << 16);
	rng_next(&rc->rng);

	struct obs_audio_info oai = {0};
	if (obs_get_audio_info(&oai)) {
		rc->speakers = oai.speakers;
		rc->samples_per_sec = oai.samples_per_sec;
	} else {
		rc->speakers = SPEAKERS_STEREO;
		rc->samples_per_sec = 48000;
	}

	if (pthread_mutex_init(&rc->mutex, NULL) != 0) {
		bfree(rc);
		return NULL;
	}

	rc->loader = os_task_queue_create();

	rc->play_hotkey =
		obs_hotkey_register_source(source, "RandomClipPlayer.Play", T_("PlayRandomClip"), play_hotkey_cb, rc);
	rc->stop_hotkey =
		obs_hotkey_register_source(source, "RandomClipPlayer.Stop", T_("StopClip"), stop_hotkey_cb, rc);

	rc_update(rc, settings);
	return rc;
}

static void rc_destroy(void *data)
{
	struct random_clip *rc = data;
	cancel_loads(rc);
	os_task_queue_destroy(rc->loader); /* waits for any in-flight load */
	cancel_loads(rc);
	stop_clip(rc);

	obs_enter_graphics();
	gs_texrender_destroy(rc->texrender);
	gs_effect_destroy(rc->fade_effect);
	obs_leave_graphics();

	bfree(rc->gain_buf);
	bfree(rc->folder);
	bfree(rc->last_file);
	file_list_free(&rc->played);
	pthread_mutex_destroy(&rc->mutex);
	bfree(rc);
}

static void rc_get_defaults(obs_data_t *settings)
{
	struct obs_video_info ovi;
	uint32_t w = 1920, h = 1080;
	if (obs_get_video_info(&ovi)) {
		w = ovi.base_width;
		h = ovi.base_height;
	}

	obs_data_set_default_bool(settings, S_RECURSIVE, false);
	obs_data_set_default_int(settings, S_REPEAT_MODE, REPEAT_SHUFFLE);
	obs_data_set_default_bool(settings, S_INTERRUPT, true);
	obs_data_set_default_bool(settings, S_LOOP, false);
	obs_data_set_default_bool(settings, S_FADE_LOOP, true);
	obs_data_set_default_bool(settings, S_FADE_SINGLE, false);
	obs_data_set_default_int(settings, S_FADE, 500);
	obs_data_set_default_bool(settings, S_AUTOSTART, false);
	obs_data_set_default_bool(settings, S_STOP_ON_SCENE, true);
	obs_data_set_default_bool(settings, S_STOP_ON_HIDE, true);
	obs_data_set_default_bool(settings, S_HW_DECODE, false);
	obs_data_set_default_int(settings, S_WIDTH, w);
	obs_data_set_default_int(settings, S_HEIGHT, h);
}

/* Only show the fade duration while at least one fade toggle is on. */
static bool fade_toggles_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	bool on = obs_data_get_bool(settings, S_FADE_LOOP) || obs_data_get_bool(settings, S_FADE_SINGLE);
	obs_property_set_visible(obs_properties_get(props, S_FADE), on);
	return true;
}

static obs_properties_t *rc_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_path(props, S_FOLDER, T_("ClipFolder"), OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_bool(props, S_RECURSIVE, T_("IncludeSubfolders"));
	obs_property_t *repeat = obs_properties_add_list(props, S_REPEAT_MODE, T_("RepeatMode"), OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(repeat, T_("RepeatMode.Shuffle"), REPEAT_SHUFFLE);
	obs_property_list_add_int(repeat, T_("RepeatMode.NotTwice"), REPEAT_NOT_TWICE);
	obs_property_list_add_int(repeat, T_("RepeatMode.Allow"), REPEAT_ALLOW);
	obs_properties_add_bool(props, S_INTERRUPT, T_("Interrupt"));
	obs_properties_add_bool(props, S_LOOP, T_("Loop"));
	obs_property_t *fade_loop = obs_properties_add_bool(props, S_FADE_LOOP, T_("FadeLoop"));
	obs_property_t *fade_single = obs_properties_add_bool(props, S_FADE_SINGLE, T_("FadeSingle"));
	obs_property_t *fade =
		obs_properties_add_int_slider(props, S_FADE, T_("FadeDuration"), MIN_FADE_MS, MAX_FADE_MS, 50);
	obs_property_int_set_suffix(fade, " ms");
	obs_property_set_long_description(fade, T_("FadeDuration.Description"));
	obs_property_set_modified_callback(fade_loop, fade_toggles_changed);
	obs_property_set_modified_callback(fade_single, fade_toggles_changed);
	obs_properties_add_bool(props, S_AUTOSTART, T_("AutoStart"));
	obs_properties_add_bool(props, S_STOP_ON_SCENE, T_("StopOnSceneChange"));
	obs_properties_add_bool(props, S_STOP_ON_HIDE, T_("StopOnHide"));
	obs_properties_add_bool(props, S_HW_DECODE, T_("HardwareDecode"));
	obs_properties_add_int(props, S_WIDTH, T_("Width"), 1, 16384, 1);
	obs_properties_add_int(props, S_HEIGHT, T_("Height"), 1, 16384, 1);
	obs_properties_add_button2(props, "play_now", T_("PlayRandomClip"), play_button_cb, data);
	obs_properties_add_button2(props, "stop_now", T_("StopClip"), stop_button_cb, data);

	return props;
}

static void rc_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct random_clip *rc = data;

	/* Program scene changed: a loop session always ends here; the current
	 * clip stops too unless "stop on scene change" is turned off. */
	long sg = os_atomic_load_long(&scene_gen);
	if (sg != rc->seen_scene_gen) {
		rc->seen_scene_gen = sg;
		rc->looping = false;

		pthread_mutex_lock(&rc->mutex);
		bool stop = rc->stop_on_scene;
		pthread_mutex_unlock(&rc->mutex);

		if (stop) {
			os_atomic_set_bool(&rc->play_requested, false);
			stop_playing(rc);
		}
	}

	if (os_atomic_set_bool(&rc->stop_requested, false)) {
		os_atomic_set_bool(&rc->play_requested, false);
		os_atomic_set_bool(&rc->autostart_requested, false);
		stop_playing(rc);
	}

	if (os_atomic_set_bool(&rc->play_requested, false))
		start_playing(rc, false);

	if (os_atomic_set_bool(&rc->autostart_requested, false)) {
		pthread_mutex_lock(&rc->mutex);
		bool autostart = rc->autostart;
		pthread_mutex_unlock(&rc->mutex);
		if (autostart)
			start_playing(rc, true);
	}

	/* nothing to play (empty/missing folder): end the loop instead of retrying */
	if (os_atomic_set_bool(&rc->load_empty, false) && rc->looping) {
		rc->looping = false;
		blog(LOG_WARNING, "[Random Clip Player] Loop stopped: no clip could be loaded");
	}

	if (os_atomic_set_bool(&rc->pending_ready, false))
		install_pending(rc);

	if (!os_atomic_load_bool(&rc->has_clip)) {
		free_fade_texture(rc);
		return;
	}

	/* Unload as soon as the clip is done, then queue the next one if looping */
	obs_source_t *child = get_child_ref(rc);
	if (!child)
		return;

	bool done = clip_finished(rc, child);
	if (rc->clip_fades) {
		rc->opacity = compute_opacity(rc, child);
		os_atomic_set_long(&rc->gain, (long)(rc->opacity * (float)GAIN_SCALE + 0.5f));
	}
	obs_source_release(child);
	if (!done)
		return;

	stop_clip(rc);

	if (rc->looping) {
		rc->loop_failures = rc->clip_played ? 0 : rc->loop_failures + 1;
		if (rc->loop_failures >= MAX_LOOP_FAILURES) {
			rc->looping = false;
			blog(LOG_WARNING, "[Random Clip Player] Loop stopped: %d clips in a row failed to play",
			     MAX_LOOP_FAILURES);
		} else {
			queue_load(rc);
		}
	}
}

static void rc_hide(void *data)
{
	struct random_clip *rc = data;
	pthread_mutex_lock(&rc->mutex);
	bool stop = rc->stop_on_hide;
	pthread_mutex_unlock(&rc->mutex);
	if (stop)
		os_atomic_set_bool(&rc->stop_requested, true);
}

static bool load_fade_effect(struct random_clip *rc)
{
	if (rc->fade_effect)
		return true;
	if (rc->fade_effect_failed)
		return false;

	char *file = obs_module_file("fade.effect");
	rc->fade_effect = file ? gs_effect_create_from_file(file, NULL) : NULL;
	bfree(file);

	if (!rc->fade_effect) {
		blog(LOG_ERROR, "[Random Clip Player] Could not load fade.effect, fades are disabled");
		rc->fade_effect_failed = true;
		return false;
	}

	rc->fade_image = gs_effect_get_param_by_name(rc->fade_effect, "image");
	rc->fade_opacity = gs_effect_get_param_by_name(rc->fade_effect, "opacity");
	return true;
}

/* Draws the clip at reduced opacity: render it into a texture, the same way
 * OBS renders sources for its own transitions, then draw that texture with
 * the fade shader. Only used on fade frames. */
static void render_faded(struct random_clip *rc, obs_source_t *child, uint32_t w, uint32_t h)
{
	if (rc->opacity <= 0.0f)
		return;
	if (!load_fade_effect(rc)) {
		obs_source_video_render(child);
		return;
	}

	const enum gs_color_space space = gs_get_color_space();
	const enum gs_color_format format = gs_get_format_from_space(space);
	if (rc->texrender && gs_texrender_get_format(rc->texrender) != format) {
		gs_texrender_destroy(rc->texrender);
		rc->texrender = NULL;
	}
	if (!rc->texrender)
		rc->texrender = gs_texrender_create(format, GS_ZS_NONE);

	gs_texrender_reset(rc->texrender);
	if (gs_texrender_begin_with_color_space(rc->texrender, w, h, space)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		obs_source_video_render(child);
		gs_texrender_end(rc->texrender);
	}

	gs_texture_t *tex = gs_texrender_get_texture(rc->texrender);
	if (!tex)
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_effect_set_texture_srgb(rc->fade_image, tex);
	gs_effect_set_float(rc->fade_opacity, rc->opacity);
	while (gs_effect_loop(rc->fade_effect, "Draw"))
		gs_draw_sprite(NULL, 0, w, h);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

static void rc_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct random_clip *rc = data;

	if (!os_atomic_load_bool(&rc->has_clip))
		return;

	obs_source_t *child = get_child_ref(rc);
	if (!child)
		return;

	uint32_t w = obs_source_get_width(child);
	uint32_t h = obs_source_get_height(child);

	if (w && h && rc->cx && rc->cy) {
		/* scale to fit, centered (letterbox / pillarbox) */
		float sx = (float)rc->cx / (float)w;
		float sy = (float)rc->cy / (float)h;
		float scale = sx < sy ? sx : sy;
		float dw = (float)w * scale;
		float dh = (float)h * scale;

		gs_matrix_push();
		gs_matrix_translate3f(((float)rc->cx - dw) * 0.5f, ((float)rc->cy - dh) * 0.5f, 0.0f);
		gs_matrix_scale3f(scale, scale, 1.0f);
		if (rc->opacity < 1.0f)
			render_faded(rc, child, w, h);
		else
			obs_source_video_render(child); /* normal frames: no extra work */
		gs_matrix_pop();
	}

	obs_source_release(child);
}

static uint32_t rc_get_width(void *data)
{
	struct random_clip *rc = data;
	return rc->cx;
}

static uint32_t rc_get_height(void *data)
{
	struct random_clip *rc = data;
	return rc->cy;
}

static void rc_enum_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	struct random_clip *rc = data;
	obs_source_t *child = get_child_ref(rc);
	if (child) {
		enum_callback(rc->source, child, param);
		obs_source_release(child);
	}
}

struct obs_source_info random_clip_source_info = {
	.id = SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.get_name = rc_get_name,
	.create = rc_create,
	.destroy = rc_destroy,
	.update = rc_update,
	.get_defaults = rc_get_defaults,
	.get_properties = rc_get_properties,
	.video_tick = rc_tick,
	.video_render = rc_render,
	.get_width = rc_get_width,
	.get_height = rc_get_height,
	.hide = rc_hide,
	.enum_active_sources = rc_enum_sources,
	.enum_all_sources = rc_enum_sources,
};
