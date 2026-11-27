#pragma once

#include <obs.h>

extern struct obs_source_info random_clip_source_info;

/* Called by the frontend event handler when the program scene changes. */
void random_clip_scene_changed(void);

/* obs_source_enum_active_tree callback: flags Random Clip Player sources
 * visible in the new scene so they can auto-start. */
void random_clip_mark_autostart(obs_source_t *parent, obs_source_t *child, void *param);
