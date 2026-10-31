/*
SPDX-License-Identifier: 0BSD

gong2 Playlist Source for OBS
Copyright (C) 2026 HAEGONG

See LICENSE for permission to use, copy, modify, and distribute this software.
*/

#pragma once

/* Creates the playlist window (an OBS dock that can float as its own window),
 * adds "Tools > Playlist Window" and installs playlist::set_open_window_callback.
 * Called from obs_module_load on the UI thread. */
void playlist_window_init();

/* Disconnects all signal handlers and timers. Called from obs_module_unload. */
void playlist_window_shutdown();
