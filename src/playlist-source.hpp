/*
SPDX-License-Identifier: 0BSD

gong2 Playlist Source for OBS
Copyright (C) 2026 HAEGONG

See LICENSE for permission to use, copy, modify, and distribute this software.
*/

#pragma once

/*
 * Public API of the playlist source, shared by the source implementation
 * (playlist-source.cpp) and the playlist window (playlist-window.cpp).
 *
 * Threading: every function here may be called from any thread (normally the
 * Qt UI thread). Anything that touches the internal Media Source is queued and
 * executed on the graphics thread from the source's video_tick.
 */

#include <obs.h>

#include <string>
#include <vector>

#define PLAYLIST_SOURCE_ID "gong2_playlist_source"
/* ID used before the gong2 rename. Still registered (hidden from the Add
 * Source menu) so scene collections saved with it keep loading. */
#define PLAYLIST_SOURCE_LEGACY_ID "playlist_source"

namespace playlist {

/* One row of the playlist as stored in the "playlist" setting. */
struct EntryInfo {
	std::string path;   /* as stored in settings: file, folder or URL */
	bool is_folder = false;
	bool is_url = false;
	bool missing = false; /* local file/folder does not exist */
	/* Playable items of this entry:
	 *   file entry   -> { path }            (empty if missing)
	 *   url entry    -> { path }
	 *   folder entry -> media files of the folder's last scan (see
	 *                   rescan_folder), in the folder's custom order (see
	 *                   set_folder_order) or else sorted by file name, without
	 *                   the files removed with exclude_folder_files */
	std::vector<std::string> files;
	/* Folder entry that has a saved custom play order (not just name order). */
	bool custom_order = false;
	/* Folder entry: number of removed files (see exclude_folder_files) that
	 * currently exist in the folder, i.e. what restore_folder_files brings back. */
	int excluded_count = 0;
};

struct Snapshot {
	std::vector<EntryInfo> entries;
	int current_entry = -1; /* -1 when nothing is loaded */
	int current_file = -1;  /* index into entries[current_entry].files */
	obs_media_state state = OBS_MEDIA_STATE_NONE;
};

/* Signals added to every playlist source's signal handler.
 * Signature: "void <name>(ptr source)". They are emitted from the graphics
 * thread; receivers must marshal to their own thread. */
constexpr const char *SIGNAL_PLAYLIST_CHANGED = "playlist_changed"; /* entries or folder contents changed */
constexpr const char *SIGNAL_CURRENT_CHANGED = "current_changed";   /* current item or media state changed */

/* Registers the source type with libobs. Called from obs_module_load. */
void register_source();

/* Flushes pending resume positions to disk. Called from obs_module_unload. */
void shutdown();

bool is_playlist_source(obs_source_t *source);

/* Copies the current playlist state. Returns false if source is not a playlist source. */
bool get_snapshot(obs_source_t *source, Snapshot &out);

/* Plays entries[entry].files[file] from the beginning. Ignored if out of range. */
void play_item(obs_source_t *source, int entry, int file);

/* Replaces the playlist with the given ordered entry paths (files, folders, URLs).
 * Writes the "playlist" setting and calls obs_source_update(). The file that is
 * currently playing keeps playing (without restarting) if it is still present. */
void set_entries(obs_source_t *source, const std::vector<std::string> &paths);

/* Sets the play order of the files inside the folder entry `folder` (the path as
 * stored in the playlist). `files` are full paths as listed in EntryInfo::files;
 * only their file names are saved, in the "folder_orders" setting. Files that are
 * not in the list (e.g. added to the folder later) follow in name order. An empty
 * vector removes the saved order (back to name order); the files removed with
 * exclude_folder_files stay removed. Calls obs_source_update(); the file that is
 * currently playing keeps playing. set_entries() drops the saved orders and
 * removed files of folders that are no longer in the list. */
void set_folder_order(obs_source_t *source, const std::string &folder, const std::vector<std::string> &files);

/* Removes files from the playlist, not from the disk: they are no longer played
 * or listed as part of the folder entry `folder` (the path as stored in the
 * playlist). `files` are full paths as listed in EntryInfo::files; only their file
 * names are saved, in the "folder_orders" setting, so they stay removed across
 * restarts and rescans. Calls obs_source_update(); if the file that is currently
 * playing is removed, playback moves on to the item that takes its place. */
void exclude_folder_files(obs_source_t *source, const std::string &folder, const std::vector<std::string> &files);

/* Brings back all files removed from the folder entry `folder` with
 * exclude_folder_files. Calls obs_source_update(). */
void restore_folder_files(obs_source_t *source, const std::string &folder);

/* Re-reads the contents of the folder entry `folder` (the path as stored in the
 * playlist). Folders are scanned when they first appear in the list (source
 * creation, OBS start, or when the folder is added) and only again through this
 * call; every other update reuses the last listing. Queues the request and calls
 * obs_source_update(), so the scan runs on the graphics thread. A file that was
 * deleted from the disk is skipped when its turn comes even without a rescan. */
void rescan_folder(obs_source_t *source, const std::string &folder);

/* UI hook used by the "Open Playlist Window" button in the source properties.
 * Installed by the playlist window module; called on the UI thread. */
using OpenWindowCallback = void (*)(obs_source_t *source);
void set_open_window_callback(OpenWindowCallback cb);

} // namespace playlist
