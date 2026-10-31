/*
SPDX-License-Identifier: 0BSD

gong2 Playlist Source for OBS
Copyright (C) 2026 HAEGONG

See LICENSE for permission to use, copy, modify, and distribute this software.
*/

#include <obs-module.h>

#include "playlist-source.hpp"
#include "playlist-window.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("gong2-playlist-source", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Playlist source built on the Media Source, with a playlist window";
}

bool obs_module_load(void)
{
	playlist::register_source();
	playlist_window_init();
	blog(LOG_INFO, "[gong2-playlist-source] loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	playlist_window_shutdown();
	playlist::shutdown();
}
