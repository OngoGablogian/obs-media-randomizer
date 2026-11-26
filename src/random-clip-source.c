/*
 * Random Clip Player - OBS source
 *
 * Plays a random video file from a folder when a hotkey is pressed.
 * Each clip is loaded into a temporary private "ffmpeg_source" (the same
 * decoder OBS's own Media Source uses). As soon as the clip finishes, or is
 * stopped, that child source is destroyed, so no file handles, decoder
 * threads or frame buffers are held while idle.
 *
 * Threading model:
 *  - Hotkeys/buttons only set atomic flags.
 *  - The folder scan and media source creation run on a background worker,
 *    so disk access never stalls OBS's render thread.
 *  - video_tick (graphics thread) installs the loaded clip and unloads it
 *    when it ends. OBS itself destroys released sources on a background
 *    thread.
 *  - While idle, tick and render return after a single atomic read.
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

#define S_FOLDER "folder"
#define S_RECURSIVE "recursive"
#define S_NO_REPEAT "no_repeat"
#define S_INTERRUPT "interrupt"
#define S_STOP_ON_HIDE "stop_on_hide"
#define S_HW_DECODE "hw_decode"
#define S_WIDTH "width"
#define S_HEIGHT "height"

#define T_(s) obs_module_text(s)

#define MAX_SCAN_DEPTH 8
#define START_TIMEOUT_NS (10ULL * 1000000000ULL) /* give up if a clip never starts */
#define MIN_CLIP_AGE_NS (500ULL * 1000000ULL)    /* ignore state checks right after start */

static const char *video_exts[] = {".mp4", ".mkv", ".mov",  ".webm", ".avi",  ".flv", ".m4v",
				   ".ts",  ".mts", ".m2ts", ".mpg",  ".mpeg", ".wmv", ".gif"};

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
	char *folder;                /* protected by mutex */
	bool recursive;
	bool no_repeat;
	bool interrupt;
	bool stop_on_hide;
	bool hw_decode;
	uint32_t cx;
	uint32_t cy;

	/* graphics-thread only */
	uint64_t clip_start_ns;
	long load_gen;

	os_task_queue_t *loader;
	long gen; /* protected by mutex; bumped on stop to cancel in-flight loads */
	volatile bool loading;
	volatile bool pending_ready;
	volatile bool has_clip; /* fast idle check for tick/render */

	/* OBS audio format, read once at creation */
	enum speaker_layout speakers;
	uint32_t samples_per_sec;

	volatile bool play_requested;
	volatile bool stop_requested;

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

/* Returns a newly allocated path, or NULL if the folder has no clips. */
static char *pick_random_file(struct random_clip *rc)
{
	pthread_mutex_lock(&rc->mutex);
	char *folder = rc->folder ? bstrdup(rc->folder) : NULL;
	bool recursive = rc->recursive;
	bool no_repeat = rc->no_repeat;
	char *last = rc->last_file ? bstrdup(rc->last_file) : NULL;
	pthread_mutex_unlock(&rc->mutex);

	if (!folder || !*folder) {
		bfree(last);
		blog(LOG_WARNING, "[Random Clip Player] No folder selected");
		bfree(folder);
		return NULL;
	}

	struct file_list files = {0};
	scan_folder(folder, recursive, 0, &files);

	char *result = NULL;
	if (files.num == 0) {
		blog(LOG_WARNING, "[Random Clip Player] No video files found in '%s'", folder);
	} else {
		size_t idx = (size_t)rand() % files.num;

		/* avoid playing the same clip twice in a row */
		if (no_repeat && files.num > 1 && last && strcmp(files.items[idx], last) == 0) {
			idx = (idx + 1 + (size_t)rand() % (files.num - 1)) % files.num;
		}

		result = files.items[idx];
		files.items[idx] = NULL;
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

	char *path = pick_random_file(rc);
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
	obs_source_add_audio_capture_callback(child, forward_audio, rc);
	obs_source_add_active_child(rc->source, child);
	os_atomic_set_bool(&rc->has_clip, true);
}

static bool clip_finished(struct random_clip *rc, obs_source_t *child)
{
	uint64_t age = os_gettime_ns() - rc->clip_start_ns;
	if (age < MIN_CLIP_AGE_NS)
		return false;

	switch (obs_source_media_get_state(child)) {
	case OBS_MEDIA_STATE_ENDED:
	case OBS_MEDIA_STATE_STOPPED:
	case OBS_MEDIA_STATE_ERROR:
		return true;
	case OBS_MEDIA_STATE_PLAYING:
	case OBS_MEDIA_STATE_PAUSED:
		return false;
	default:
		/* never managed to start (bad/unreadable file) */
		return age > START_TIMEOUT_NS;
	}
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

	pthread_mutex_lock(&rc->mutex);
	bfree(rc->folder);
	rc->folder = bstrdup(obs_data_get_string(settings, S_FOLDER));
	rc->recursive = obs_data_get_bool(settings, S_RECURSIVE);
	rc->no_repeat = obs_data_get_bool(settings, S_NO_REPEAT);
	rc->interrupt = obs_data_get_bool(settings, S_INTERRUPT);
	rc->stop_on_hide = obs_data_get_bool(settings, S_STOP_ON_HIDE);
	rc->hw_decode = obs_data_get_bool(settings, S_HW_DECODE);
	rc->cx = (uint32_t)obs_data_get_int(settings, S_WIDTH);
	rc->cy = (uint32_t)obs_data_get_int(settings, S_HEIGHT);
	pthread_mutex_unlock(&rc->mutex);
}

static void *rc_create(obs_data_t *settings, obs_source_t *source)
{
	struct random_clip *rc = bzalloc(sizeof(*rc));
	rc->source = source;

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
	bfree(rc->folder);
	bfree(rc->last_file);
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
	obs_data_set_default_bool(settings, S_NO_REPEAT, true);
	obs_data_set_default_bool(settings, S_INTERRUPT, true);
	obs_data_set_default_bool(settings, S_STOP_ON_HIDE, true);
	obs_data_set_default_bool(settings, S_HW_DECODE, false);
	obs_data_set_default_int(settings, S_WIDTH, w);
	obs_data_set_default_int(settings, S_HEIGHT, h);
}

static obs_properties_t *rc_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_path(props, S_FOLDER, T_("ClipFolder"), OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_bool(props, S_RECURSIVE, T_("IncludeSubfolders"));
	obs_properties_add_bool(props, S_NO_REPEAT, T_("NoRepeat"));
	obs_properties_add_bool(props, S_INTERRUPT, T_("Interrupt"));
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

	if (os_atomic_set_bool(&rc->stop_requested, false)) {
		os_atomic_set_bool(&rc->play_requested, false);
		cancel_loads(rc);
		stop_clip(rc);
	}

	if (os_atomic_set_bool(&rc->play_requested, false)) {
		pthread_mutex_lock(&rc->mutex);
		bool interrupt = rc->interrupt;
		pthread_mutex_unlock(&rc->mutex);

		bool playing = os_atomic_load_bool(&rc->has_clip);
		if ((!playing || interrupt) && rc->loader && !os_atomic_set_bool(&rc->loading, true)) {
			pthread_mutex_lock(&rc->mutex);
			rc->load_gen = rc->gen;
			pthread_mutex_unlock(&rc->mutex);
			if (!os_task_queue_queue_task(rc->loader, load_task, rc))
				os_atomic_set_bool(&rc->loading, false);
		}
	}

	if (os_atomic_set_bool(&rc->pending_ready, false))
		install_pending(rc);

	if (!os_atomic_load_bool(&rc->has_clip))
		return;

	/* Unload as soon as the clip is done */
	obs_source_t *child = get_child_ref(rc);
	if (child) {
		bool done = clip_finished(rc, child);
		obs_source_release(child);
		if (done)
			stop_clip(rc);
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
		obs_source_video_render(child);
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
	.id = "random_clip_player",
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
