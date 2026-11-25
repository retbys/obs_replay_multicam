#pragma once
#include <obs-module.h>
char **obs_frontend_get_scene_names(void);
void obs_frontend_set_current_scene(obs_source_t *scene);
