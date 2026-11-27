#pragma once

#include <obs.h>

extern struct obs_source_info media_randomizer_source_info;

/* Called by the frontend event handler when the program scene changes. */
void media_randomizer_scene_changed(void);

/* obs_source_enum_active_tree callback: flags Media Randomizer sources
 * visible in the new scene so they can auto-start. */
void media_randomizer_mark_autostart(obs_source_t *parent, obs_source_t *child, void *param);
