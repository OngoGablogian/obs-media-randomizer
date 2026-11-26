/*
 * Random Clip Player - OBS source
 *
 * Plays a random video file from a folder when a hotkey is pressed.
 * Each clip is loaded into a temporary private "ffmpeg_source" (the same
 * decoder OBS's own Media Source uses). As soon as the clip finishes, or is
 * stopped, that child source is destroyed, so no file handles, decoder
 * threads or frame buffers are held while idle.
 *
 * Threading model: hotkeys/buttons only set atomic flags. All creation and
 * destruction of the child source happens in video_tick (graphics thread).
 * The render and audio threads take a reference to the child under a mutex.
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <util/dstr.h>

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
	obs_source_t *child; /* protected by mutex */
	char *folder;        /* protected by mutex */
	bool recursive;
	bool no_repeat;
	bool interrupt;
	bool stop_on_hide;
	bool hw_decode;
	uint32_t cx;
	uint32_t cy;

	/* graphics-thread only */
	char *last_file;
	uint64_t clip_start_ns;

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
	pthread_mutex_unlock(&rc->mutex);

	if (!folder || !*folder) {
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
		if (no_repeat && files.num > 1 && rc->last_file && strcmp(files.items[idx], rc->last_file) == 0) {
			idx = (idx + 1 + (size_t)rand() % (files.num - 1)) % files.num;
		}

		result = files.items[idx];
		files.items[idx] = NULL;
	}

	file_list_free(&files);
	bfree(folder);
	return result;
}

/* ------------------------------------------------------------------------- */
/* Child (clip) management - graphics thread only                             */

static obs_source_t *get_child_ref(struct random_clip *rc)
{
	pthread_mutex_lock(&rc->mutex);
	obs_source_t *child = rc->child ? obs_source_get_ref(rc->child) : NULL;
	pthread_mutex_unlock(&rc->mutex);
	return child;
}

static void stop_clip(struct random_clip *rc)
{
	pthread_mutex_lock(&rc->mutex);
	obs_source_t *child = rc->child;
	rc->child = NULL;
	pthread_mutex_unlock(&rc->mutex);

	if (child) {
		obs_source_remove_active_child(rc->source, child);
		obs_source_release(child); /* destroys the decoder and closes the file */
	}
}

static void start_clip(struct random_clip *rc)
{
	char *path = pick_random_file(rc);
	if (!path)
		return;

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
	obs_data_set_bool(settings, "hw_decode", hw_decode);
	obs_data_set_int(settings, "speed_percent", 100);

	obs_source_t *child = obs_source_create_private("ffmpeg_source", "Random Clip Player (clip)", settings);
	obs_data_release(settings);

	if (!child) {
		blog(LOG_ERROR, "[Random Clip Player] Failed to create media source for '%s'", path);
		bfree(path);
		return;
	}

	blog(LOG_INFO, "[Random Clip Player] Playing '%s'", path);

	bfree(rc->last_file);
	rc->last_file = path;
	rc->clip_start_ns = os_gettime_ns();

	pthread_mutex_lock(&rc->mutex);
	rc->child = child;
	pthread_mutex_unlock(&rc->mutex);

	obs_source_add_active_child(rc->source, child);
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

	if (pthread_mutex_init(&rc->mutex, NULL) != 0) {
		bfree(rc);
		return NULL;
	}

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
		stop_clip(rc);
	}

	if (os_atomic_set_bool(&rc->play_requested, false)) {
		pthread_mutex_lock(&rc->mutex);
		bool playing = rc->child != NULL;
		bool interrupt = rc->interrupt;
		pthread_mutex_unlock(&rc->mutex);

		if (!playing || interrupt) {
			stop_clip(rc);
			start_clip(rc);
		}
	}

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

static bool rc_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output, uint32_t mixers,
			    size_t channels, size_t sample_rate)
{
	UNUSED_PARAMETER(sample_rate);
	struct random_clip *rc = data;

	obs_source_t *child = get_child_ref(rc);
	if (!child)
		return false;

	bool success = false;
	if (!obs_source_audio_pending(child)) {
		uint64_t ts = obs_source_get_audio_timestamp(child);
		if (ts) {
			struct obs_source_audio_mix child_audio;
			obs_source_get_audio_mix(child, &child_audio);

			for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
				if ((mixers & (1 << mix)) == 0)
					continue;
				for (size_t ch = 0; ch < channels; ch++) {
					float *out = audio_output->output[mix].data[ch];
					const float *in = child_audio.output[mix].data[ch];
					if (out && in)
						memcpy(out, in, AUDIO_OUTPUT_FRAMES * sizeof(float));
				}
			}

			*ts_out = ts;
			success = true;
		}
	}

	obs_source_release(child);
	return success;
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
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE |
			OBS_SOURCE_DO_NOT_DUPLICATE,
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
	.audio_render = rc_audio_render,
	.enum_active_sources = rc_enum_sources,
	.enum_all_sources = rc_enum_sources,
};
