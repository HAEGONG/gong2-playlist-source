/*
gong2 Playlist Source for OBS
Copyright (C) 2026

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

/* Creates the playlist window (an OBS dock that can float as its own window),
 * adds "Tools > Playlist Window" and installs playlist::set_open_window_callback.
 * Called from obs_module_load on the UI thread. */
void playlist_window_init();

/* Disconnects all signal handlers and timers. Called from obs_module_unload. */
void playlist_window_shutdown();
