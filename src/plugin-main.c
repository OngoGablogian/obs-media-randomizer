#include <obs-module.h>
#include <obs-frontend-api.h>

#include "random-clip-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-random-clip", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Plays a random video clip from a folder when a hotkey is pressed.";
}

/* Runs on the UI thread, only when an OBS event happens. */
static void frontend_event(enum obs_frontend_event event, void *param)
{
	UNUSED_PARAMETER(param);

	if (event != OBS_FRONTEND_EVENT_SCENE_CHANGED)
		return;

	/* tell every Random Clip Player that the scene changed first, then flag
	 * the ones visible in the new scene for auto-start */
	random_clip_scene_changed();

	obs_source_t *scene = obs_frontend_get_current_scene();
	if (scene) {
		obs_source_enum_active_tree(scene, random_clip_mark_autostart, NULL);
		obs_source_release(scene);
	}
}

bool obs_module_load(void)
{
	obs_register_source(&random_clip_source_info);
	obs_frontend_add_event_callback(frontend_event, NULL);
	blog(LOG_INFO, "[Random Clip Player] loaded");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, NULL);
}
