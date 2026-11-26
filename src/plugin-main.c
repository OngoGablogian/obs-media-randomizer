#include <obs-module.h>
#include <util/platform.h>
#include <stdlib.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-random-clip-player", "en-US")

extern struct obs_source_info random_clip_source_info;

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Plays a random video clip from a folder when a hotkey is pressed.";
}

bool obs_module_load(void)
{
	srand((unsigned int)os_gettime_ns());
	obs_register_source(&random_clip_source_info);
	blog(LOG_INFO, "[Random Clip Player] loaded");
	return true;
}
