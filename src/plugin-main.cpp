/*
gong2 Playlist Source for OBS
Copyright (C) 2026

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
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
