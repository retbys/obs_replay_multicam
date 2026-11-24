/*
Multicam Replay for OBS Studio
Copyright (C) 2026

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>
#include "mc-common.h"
#include "mc-api.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Multi-camera instant replay (low CPU/GPU, raw frame ring buffer in RAM)";
}

void obs_module_post_load(void)
{
	/* меню OBS доступно только после загрузки всех модулей */
	mc_control_init();
}

bool obs_module_load(void)
{
	mc_pool_init();
	mc_registry_init();
	mc_replay_registry_init();
	obs_register_source(&mc_capture_filter_info);
	obs_register_source(&mc_replay_source_info);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	mc_control_shutdown();
	mc_replay_registry_shutdown();
	mc_registry_shutdown();
	mc_pool_shutdown();
	obs_log(LOG_INFO, "plugin unloaded");
}
