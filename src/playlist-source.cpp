/*
gong2 Playlist Source for OBS
Copyright (C) 2026

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

/*
 * Playlist Source: plays a list of files, folders and URLs one after another
 * through a private, internal Media Source ("ffmpeg_source").
 *
 * Threading model
 * ---------------
 * Everything that manipulates the internal Media Source runs on the graphics
 * thread: update() (deferred to the video tick for VIDEO sources), the media_*
 * callbacks (queued by libobs and executed in the video tick), activate /
 * deactivate and video_tick. The UI thread only reads state through
 * get_snapshot() or queues work (play_item, set_entries, obs_source_media_*).
 * Signals of the child arrive on the graphics or the media thread; they only
 * set atomic flags that video_tick turns into actions.
 *
 * libobs behaviors this file is built around
 * ------------------------------------------
 *  - obs_source_update() / obs_source_media_*() on the child are not applied
 *    immediately: both run in the child's next video tick (media actions
 *    first, then the deferred update). A seek must therefore wait for the
 *    child's "media_started" signal; seeking right after changing the path
 *    would seek the old file.
 *  - ffmpeg_source only reopens its media when the path (or hw_decode / speed)
 *    changes. Replaying the same path needs a two-phase reload (empty path
 *    first, real path one frame later).
 *  - Destroying the old media inside the child's update can emit "media_ended"
 *    synchronously, so "ended" must be ignored while a load is in flight.
 *  - The child's audio capture callback runs with the child's filter_mutex
 *    held, while rendering takes the parent's filter_mutex and then the
 *    child's (get_width). Calling obs_source_output_audio(parent) from the
 *    callback therefore deadlocks; the audio is queued and output in
 *    video_tick instead.
 */

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "playlist-source.hpp"
#include "resume-store.hpp"

#define LOG_PREFIX "[gong2-playlist-source] "

namespace fs = std::filesystem;

namespace playlist {

namespace {

/* ------------------------------------------------------------------------- */
/* Constants                                                                  */
/* ------------------------------------------------------------------------- */

constexpr const char *kChildId = "ffmpeg_source";
constexpr const char *kChildName = "gong2_playlist_source_media";

constexpr uint64_t kLoadTimeoutNs = 5ULL * 1000000000ULL; /* local files only */
constexpr uint64_t kResumeIntervalNs = 5ULL * 1000000000ULL;
constexpr size_t kMaxQueuedAudio = 256; /* chunks; ~5 s at 1024 frames / 48 kHz */

enum VisibilityBehavior {
	kAlwaysPlay = 0,
	kPauseUnpause = 1,
	kStopRestart = 2,
};

const char *const kVideoExts[] = {"mp4", "mpg", "mpeg", "m4v", "ts",  "mts", "m2ts", "mov",
				  "mxf", "flv", "mkv",  "avi", "gif", "webm", "wmv",  "3gp"};
const char *const kAudioExts[] = {"mp3", "m4a", "mka", "aac", "ogg", "oga", "wav", "opus", "flac", "wma"};

std::atomic<OpenWindowCallback> g_open_window_cb{nullptr};

inline const char *tr(const char *key)
{
	return obs_module_text(key);
}

/* ------------------------------------------------------------------------- */
/* Entry scanning                                                             */
/* ------------------------------------------------------------------------- */

bool is_url(const std::string &s)
{
	return s.find("://") != std::string::npos;
}

bool has_media_ext(std::string ext)
{
	if (!ext.empty() && ext[0] == '.')
		ext.erase(0, 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	for (const char *e : kVideoExts)
		if (ext == e)
			return true;
	for (const char *e : kAudioExts)
		if (ext == e)
			return true;
	return false;
}

bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

char ascii_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive "natural" comparison: digit runs compare by numeric value,
 * so "ep2" sorts before "ep10". Returns <0, 0 or >0. */
int natural_compare(const std::string &a, const std::string &b)
{
	size_t i = 0, j = 0;
	while (i < a.size() && j < b.size()) {
		if (is_digit(a[i]) && is_digit(b[j])) {
			size_t si = i, sj = j;
			while (si < a.size() && a[si] == '0')
				++si;
			while (sj < b.size() && b[sj] == '0')
				++sj;
			size_t ei = si, ej = sj;
			while (ei < a.size() && is_digit(a[ei]))
				++ei;
			while (ej < b.size() && is_digit(b[ej]))
				++ej;
			const size_t la = ei - si, lb = ej - sj;
			if (la != lb)
				return la < lb ? -1 : 1;
			const int c = a.compare(si, la, b, sj, lb);
			if (c != 0)
				return c < 0 ? -1 : 1;
			i = ei;
			j = ej;
		} else {
			const char ca = ascii_lower(a[i]), cb = ascii_lower(b[j]);
			if (ca != cb)
				return (unsigned char)ca < (unsigned char)cb ? -1 : 1;
			++i;
			++j;
		}
	}
	if (i < a.size())
		return 1;
	if (j < b.size())
		return -1;
	return 0;
}

/* Lists the media files directly inside a folder (no recursion, no dot files). */
std::vector<std::string> scan_folder(const fs::path &dir)
{
	struct Found {
		std::string name;
		std::string path;
	};
	std::vector<Found> found;

	std::error_code ec;
	fs::directory_iterator it(dir, ec);
	const fs::directory_iterator end;
	while (!ec && it != end) {
		const fs::path &p = it->path();
		const std::string name = p.filename().u8string();
		std::error_code ec2;
		if (!name.empty() && name[0] != '.' && has_media_ext(p.extension().u8string()) &&
		    it->is_regular_file(ec2))
			found.push_back({name, p.u8string()});
		it.increment(ec);
	}

	std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
		const int c = natural_compare(a.name, b.name);
		return c != 0 ? c < 0 : a.name < b.name;
	});

	std::vector<std::string> out;
	out.reserve(found.size());
	for (Found &f : found)
		out.push_back(std::move(f.path));
	return out;
}

/* Saved state of one folder entry: file names only. */
struct FolderState {
	std::string folder;                 /* as stored in the playlist entry's "value" */
	std::vector<std::string> names;     /* custom play order */
	std::vector<std::string> excluded;  /* files removed from the playlist (never from the disk) */
};

/* Reads a {"name": ...} array of a "folder_orders" record. */
std::vector<std::string> read_name_array(obs_data_t *item, const char *key)
{
	std::vector<std::string> out;
	obs_data_array_t *array = obs_data_get_array(item, key);
	if (!array)
		return out;

	const size_t n = obs_data_array_count(array);
	for (size_t j = 0; j < n; j++) {
		obs_data_t *f = obs_data_array_item(array, j);
		const char *name = obs_data_get_string(f, "name");
		if (name && *name)
			out.push_back(name);
		obs_data_release(f);
	}
	obs_data_array_release(array);
	return out;
}

/* Builds a {"name": ...} array. The caller releases it. */
obs_data_array_t *make_name_array(const std::vector<std::string> &names)
{
	obs_data_array_t *array = obs_data_array_create();
	for (const std::string &name : names) {
		obs_data_t *f = obs_data_create();
		obs_data_set_string(f, "name", name.c_str());
		obs_data_array_push_back(array, f);
		obs_data_release(f);
	}
	return array;
}

/* Reads the "folder_orders" setting. Records saved before files could be
 * removed only have "files". */
std::vector<FolderState> read_folder_states(obs_data_t *settings)
{
	std::vector<FolderState> out;
	obs_data_array_t *array = obs_data_get_array(settings, "folder_orders");
	if (!array)
		return out;

	const size_t count = obs_data_array_count(array);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *folder = obs_data_get_string(item, "folder");
		if (folder && *folder) {
			FolderState st;
			st.folder = folder;
			st.names = read_name_array(item, "files");
			st.excluded = read_name_array(item, "excluded");
			out.push_back(std::move(st));
		}
		obs_data_release(item);
	}
	obs_data_array_release(array);
	return out;
}

/* Builds the "folder_orders" setting value. Records with neither an order nor
 * removed files are left out. The caller releases the array. */
obs_data_array_t *make_folder_states_array(const std::vector<FolderState> &states)
{
	obs_data_array_t *array = obs_data_array_create();
	for (const FolderState &st : states) {
		if (st.names.empty() && st.excluded.empty())
			continue;
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "folder", st.folder.c_str());
		if (!st.names.empty()) {
			obs_data_array_t *files = make_name_array(st.names);
			obs_data_set_array(item, "files", files);
			obs_data_array_release(files);
		}
		if (!st.excluded.empty()) {
			obs_data_array_t *excluded = make_name_array(st.excluded);
			obs_data_set_array(item, "excluded", excluded);
			obs_data_array_release(excluded);
		}
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	return array;
}

/* Applies a saved order to a folder's file names (given in natural order).
 * Returns indices into `names`: first the saved names that still exist, in
 * saved order, then the remaining files in their natural order. Saved names
 * that no longer exist (and repeated ones) are ignored. */
std::vector<size_t> apply_saved_order(const std::vector<std::string> &names, const std::vector<std::string> &saved)
{
	std::vector<size_t> out;
	out.reserve(names.size());
	std::vector<char> used(names.size(), 0);
	for (const std::string &s : saved) {
		for (size_t i = 0; i < names.size(); i++) {
			if (!used[i] && names[i] == s) {
				used[i] = 1;
				out.push_back(i);
				break;
			}
		}
	}
	for (size_t i = 0; i < names.size(); i++)
		if (!used[i])
			out.push_back(i);
	return out;
}

/* Play order of a folder's file names (given in natural order): the saved order
 * (see apply_saved_order) without the excluded names. Returns indices into
 * `names`. `excluded_count` gets the number of excluded names that exist in
 * `names`; excluded names that no longer exist are ignored. */
std::vector<size_t> arrange_folder_files(const std::vector<std::string> &names, const std::vector<std::string> &saved,
					 const std::vector<std::string> &excluded, int &excluded_count)
{
	const std::unordered_set<std::string> skip(excluded.begin(), excluded.end());
	std::vector<size_t> out;
	out.reserve(names.size());
	excluded_count = 0;
	for (size_t i : apply_saved_order(names, saved)) {
		if (skip.count(names[i]))
			excluded_count++;
		else
			out.push_back(i);
	}
	return out;
}

std::string file_name_of(const std::string &path)
{
	return fs::u8path(path).filename().u8string();
}

/* Folder path -> media files (full paths, natural order) from the last scan. */
using FolderCache = std::map<std::string, std::vector<std::string>>;

/* A folder is scanned when it is not in `cache` yet or is listed in `rescan`;
 * otherwise the cached listing is used. */
EntryInfo make_entry(const std::string &value, const std::vector<FolderState> &states, FolderCache &cache,
		     const std::set<std::string> &rescan)
{
	EntryInfo e;
	e.path = value;

	if (is_url(value)) {
		e.is_url = true;
		e.files.push_back(value);
		return e;
	}

	const fs::path p = fs::u8path(value);
	std::error_code ec;
	if (fs::is_directory(p, ec)) {
		e.is_folder = true;
		auto cached = cache.find(value);
		if (cached == cache.end() || rescan.count(value))
			cached = cache.insert_or_assign(value, scan_folder(p)).first;
		const std::vector<std::string> &scanned = cached->second;

		const FolderState *state = nullptr;
		for (const FolderState &st : states) {
			if (st.folder == value) {
				state = &st;
				break;
			}
		}

		if (!state) {
			e.files = scanned;
			return e;
		}

		/* A saved custom order overrides the name order; removed files are dropped. */
		std::vector<std::string> names;
		names.reserve(scanned.size());
		for (const std::string &f : scanned)
			names.push_back(file_name_of(f));
		e.files.reserve(scanned.size());
		for (size_t i : arrange_folder_files(names, state->names, state->excluded, e.excluded_count))
			e.files.push_back(scanned[i]);
		e.custom_order = !state->names.empty();
		return e;
	}

	cache.erase(value);
	ec.clear();
	if (fs::exists(p, ec))
		e.files.push_back(value);
	else
		e.missing = true;
	return e;
}

/* Builds the entry list from the settings. Folder listings come from `cache`
 * (see make_entry); afterwards the cache only keeps folders that are in the list. */
std::vector<EntryInfo> build_entries(obs_data_t *settings, FolderCache &cache, const std::set<std::string> &rescan)
{
	std::vector<EntryInfo> entries;
	obs_data_array_t *array = obs_data_get_array(settings, "playlist");
	if (array) {
		const std::vector<FolderState> states = read_folder_states(settings);
		const size_t count = obs_data_array_count(array);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(array, i);
			const char *value = obs_data_get_string(item, "value");
			if (value && *value)
				entries.push_back(make_entry(value, states, cache, rescan));
			obs_data_release(item);
		}
		obs_data_array_release(array);
	}

	for (auto it = cache.begin(); it != cache.end();) {
		const bool listed = std::any_of(entries.begin(), entries.end(), [&](const EntryInfo &e) {
			return e.is_folder && e.path == it->first;
		});
		it = listed ? std::next(it) : cache.erase(it);
	}
	return entries;
}

bool file_exists(const std::string &path)
{
	std::error_code ec;
	return fs::exists(fs::u8path(path), ec);
}

std::string display_name(const std::string &path, bool url)
{
	if (url)
		return path;
	const std::string name = fs::u8path(path).filename().u8string();
	return name.empty() ? path : name;
}

/* ------------------------------------------------------------------------- */
/* The source                                                                 */
/* ------------------------------------------------------------------------- */

/* A playable item: entries[entry].files[file]. */
struct Item {
	int entry = -1;
	int file = -1;
	bool valid() const { return entry >= 0 && file >= 0; }
	bool operator==(const Item &o) const { return entry == o.entry && file == o.file; }
	bool operator!=(const Item &o) const { return !(*this == o); }
};

struct PlaylistSource;

/* One block of the child's audio, copied out of the capture callback. */
struct AudioChunk {
	uint32_t frames = 0;
	uint64_t timestamp = 0;
	std::vector<float> planes[MAX_AV_PLANES];
};

/* Context for one hotkey callback. */
struct HotkeyCtx {
	PlaylistSource *ps = nullptr;
	int action = 0;
};

enum HotkeyAction { kHkPlayPause, kHkRestart, kHkStop, kHkNext, kHkPrevious, kHkCount };

struct PlaylistSource {
	obs_source_t *source = nullptr;
	obs_source_t *child = nullptr;

	/* Settings (written by update only). */
	bool loop = true;
	bool shuffle = false;
	bool resume_on_startup = true;
	bool hw_decode = false;
	int visibility = kPauseUnpause;
	int speed = 100;

	/* Shared with the UI thread; guarded by `mutex`. Never emit signals
	 * while holding it. Only the graphics thread writes these, so the
	 * graphics thread may read them without the lock. */
	std::mutex mutex;
	std::vector<EntryInfo> entries;
	int cur_entry = -1;
	int cur_file = -1;
	bool has_request = false; /* play_item() request waiting for video_tick */
	int req_entry = -1;
	int req_file = -1;
	std::set<std::string> rescan_pending; /* folders rescan_folder() wants re-read by the next update */

	/* Graphics thread only. */
	bool first_update = true;
	std::map<std::string, std::vector<std::string>> folder_cache; /* folder -> files of its last scan */
	std::vector<Item> flat;                /* all playable items in list order */
	std::vector<std::string> flat_paths;   /* paths of `flat`, to detect list changes */
	std::vector<Item> order;               /* shuffled permutation of `flat` */
	size_t order_pos = 0;
	std::mt19937 rng{std::random_device{}()};

	/* What was last pushed to the child's settings. */
	std::string child_path;
	bool child_is_url = false;
	bool child_looping = false;
	bool child_hw = false;
	int child_speed = 100;

	/* Two-phase reload of the same path (see the file comment). */
	bool has_phase2 = false;
	std::string phase2_path;
	bool phase2_url = false;
	uint64_t phase2_due = 0;
	uint64_t frames_done = 0; /* completed video ticks */

	/* Child pushes within the current frame: the child only sees the last
	 * one, so an A -> B -> A sequence inside one frame never reopens A. */
	bool pushed_in_frame = false;
	std::string frame_base_path;
	bool frame_base_url = false;

	uint64_t load_start_ns = 0;
	uint64_t last_resume_ns;

	bool cur_dirty = false;  /* emit current_changed at the end of the tick */
	bool list_dirty = false; /* emit playlist_changed at the end of the tick */

	/* Read by other threads (get_state, save, ...). */
	std::atomic<bool> loading{false};
	std::atomic<int64_t> pending_seek_ms{0};
	std::atomic<bool> pending_pause{false};
	std::atomic<bool> paused_by_hide{false};
	std::atomic<bool> stopped_by_hide{false};
	std::atomic<bool> self_stopped{false}; /* we stopped the child (user stop / hide) */
	std::atomic<size_t> playable_count{0};

	/* Set from child signals (graphics or media thread). */
	std::atomic<bool> sig_started{false};
	std::atomic<bool> sig_ended{false};
	std::atomic<bool> sig_state{false};

	HotkeyCtx hotkeys[kHkCount];

	/* Child audio waiting to be output by video_tick (see the file comment). */
	std::mutex audio_mutex;
	std::deque<AudioChunk> audio_queue;

	explicit PlaylistSource(obs_source_t *src) : source(src), last_resume_ns(os_gettime_ns()) {}

	/* -- helpers ---------------------------------------------------------- */

	void read_config(obs_data_t *s)
	{
		loop = obs_data_get_bool(s, "loop");
		shuffle = obs_data_get_bool(s, "shuffle");
		visibility = (int)obs_data_get_int(s, "visibility_behavior");
		resume_on_startup = obs_data_get_bool(s, "resume_on_startup");
		hw_decode = obs_data_get_bool(s, "hw_decode");
		speed = (int)obs_data_get_int(s, "speed_percent");
		if (speed < 1 || speed > 200)
			speed = 100;
	}

	Item current() const { return {cur_entry, cur_file}; }

	void set_current(Item it)
	{
		std::lock_guard<std::mutex> lock(mutex);
		cur_entry = it.entry;
		cur_file = it.file;
		cur_dirty = true;
	}

	const std::string &path_of(Item it) const { return entries[it.entry].files[it.file]; }

	bool in_range(Item it) const
	{
		return it.valid() && (size_t)it.entry < entries.size() &&
		       (size_t)it.file < entries[it.entry].files.size();
	}

	/* In range, and a local file that still exists (URLs always count). */
	bool playable(Item it) const
	{
		if (!in_range(it))
			return false;
		return entries[it.entry].is_url || file_exists(path_of(it));
	}

	static std::vector<Item> make_flat(const std::vector<EntryInfo> &list)
	{
		std::vector<Item> out;
		for (size_t e = 0; e < list.size(); e++)
			for (size_t f = 0; f < list[e].files.size(); f++)
				out.push_back({(int)e, (int)f});
		return out;
	}

	int flat_index(Item it) const
	{
		for (size_t i = 0; i < flat.size(); i++)
			if (flat[i] == it)
				return (int)i;
		return -1;
	}

	/* Finds `path` in `list`, preferring entry `prefer_entry`. */
	static Item find_path(const std::vector<EntryInfo> &list, const std::string &path, int prefer_entry)
	{
		if (prefer_entry >= 0 && (size_t)prefer_entry < list.size()) {
			const auto &files = list[prefer_entry].files;
			for (size_t f = 0; f < files.size(); f++)
				if (files[f] == path)
					return {prefer_entry, (int)f};
		}
		for (size_t e = 0; e < list.size(); e++)
			for (size_t f = 0; f < list[e].files.size(); f++)
				if (list[e].files[f] == path)
					return {(int)e, (int)f};
		return {};
	}

	bool want_looping() const { return loop && flat.size() == 1; }

	/* Not on the program output while visibility handling is enabled. */
	bool hidden_now() const { return visibility != kAlwaysPlay && !obs_source_active(source); }

	obs_media_state state() const
	{
		if (playable_count.load() == 0)
			return OBS_MEDIA_STATE_NONE;
		if (loading.load())
			return OBS_MEDIA_STATE_OPENING;
		return obs_source_media_get_state(child);
	}

	std::string now_playing_text()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (cur_entry >= 0 && cur_file >= 0 && (size_t)cur_entry < entries.size() &&
		    (size_t)cur_file < entries[cur_entry].files.size())
			return display_name(entries[cur_entry].files[cur_file], entries[cur_entry].is_url);
		return tr("NothingPlaying");
	}

	void emit(const char *signal)
	{
		calldata_t cd = {};
		calldata_set_ptr(&cd, "source", source);
		signal_handler_signal(obs_source_get_signal_handler(source), signal, &cd);
		calldata_free(&cd);
	}

	/* Emits the pending signals. Called without holding `mutex`. */
	void flush_signals()
	{
		const bool list = list_dirty, cur = cur_dirty;
		list_dirty = cur_dirty = false;
		if (list)
			emit(SIGNAL_PLAYLIST_CHANGED);
		if (cur)
			emit(SIGNAL_CURRENT_CHANGED);
	}

	/* -- shuffle order ---------------------------------------------------- */

	/* New random permutation with the current item (if any) first. */
	void rebuild_order()
	{
		order = flat;
		std::shuffle(order.begin(), order.end(), rng);
		const int idx = [&] {
			for (size_t i = 0; i < order.size(); i++)
				if (order[i] == current())
					return (int)i;
			return -1;
		}();
		if (idx > 0)
			std::swap(order[0], order[idx]);
		order_pos = 0;
	}

	/* Makes order_pos point at the current item, e.g. after the user jumped to it. */
	void sync_order()
	{
		if (!shuffle)
			return;
		if (order.size() != flat.size() || order_pos >= order.size()) {
			rebuild_order();
			return;
		}
		if (order[order_pos] == current())
			return;
		for (size_t i = 0; i < order.size(); i++) {
			if (order[i] == current()) {
				order_pos = i;
				return;
			}
		}
		if (current().valid())
			rebuild_order();
	}

	/* The permutation is used up: reshuffle, avoiding an immediate repeat. */
	void reshuffle_after_exhausted()
	{
		const Item last = order.empty() ? Item() : order.back();
		order = flat;
		std::shuffle(order.begin(), order.end(), rng);
		if (order.size() > 1 && order[0] == last)
			std::swap(order[0], order[1 + rng() % (order.size() - 1)]);
		order_pos = 0;
	}

	Item first_item()
	{
		if (flat.empty())
			return {};
		if (shuffle) {
			if (order.size() != flat.size())
				rebuild_order();
			return order[0];
		}
		return flat[0];
	}

	/* Finds the item after (dir > 0) or before (dir < 0) the current one in
	 * playback order. Wraps around when `wrap`; returns false at the ends
	 * otherwise. In shuffle mode this also moves order_pos. */
	bool step(int dir, bool wrap, Item &out)
	{
		const long long n = (long long)flat.size();
		if (n == 0)
			return false;

		if (!shuffle) {
			const int idx = flat_index(current());
			long long next = idx < 0 ? (dir > 0 ? 0 : n - 1) : idx + dir;
			if (next >= n) {
				if (!wrap)
					return false;
				next = 0;
			} else if (next < 0) {
				if (!wrap)
					return false;
				next = n - 1;
			}
			out = flat[(size_t)next];
			return true;
		}

		if ((long long)order.size() != n)
			rebuild_order();

		long long pos;
		if (!current().valid()) {
			pos = dir > 0 ? 0 : n - 1;
		} else {
			pos = (long long)order_pos + dir;
			if (pos >= n) {
				if (!wrap)
					return false;
				reshuffle_after_exhausted();
				pos = 0;
			} else if (pos < 0) {
				if (!wrap)
					return false;
				pos = n - 1;
			}
		}
		order_pos = (size_t)pos;
		out = order[(size_t)pos];
		return true;
	}

	/* -- child control ---------------------------------------------------- */

	/* Writes path + hw/speed/looping into the child's settings. The child
	 * applies them in its next tick. */
	void push_child(const std::string &path, bool url)
	{
		if (!pushed_in_frame) {
			pushed_in_frame = true;
			frame_base_path = child_path;
			frame_base_url = child_is_url;
		}

		obs_data_t *d = obs_data_create();
		obs_data_set_bool(d, "is_local_file", !url);
		obs_data_set_string(d, "local_file", url ? "" : path.c_str());
		obs_data_set_string(d, "input", url ? path.c_str() : "");
		obs_data_set_bool(d, "looping", want_looping());
		obs_data_set_bool(d, "hw_decode", hw_decode);
		obs_data_set_int(d, "speed_percent", speed);
		obs_source_update(child, d);
		obs_data_release(d);

		child_path = path;
		child_is_url = url;
		child_looping = want_looping();
		child_hw = hw_decode;
		child_speed = speed;
	}

	/* Empties the child (no media) and forgets everything about playback. */
	void clear_child()
	{
		has_phase2 = false;
		loading = false;
		pending_seek_ms = 0;
		pending_pause = false;
		paused_by_hide = false;
		stopped_by_hide = false;
		self_stopped = false;
		sig_started = false;
		sig_ended = false;
		if (!child_path.empty() || child_is_url)
			push_child("", false);
	}

	/* Starts playing `it` (skipping local files that vanished). The seek and
	 * pause are applied once the child reports "media_started". */
	void load(Item it, int64_t start_ms, bool paused, bool by_hide = false)
	{
		if (!it.valid()) {
			clear_child();
			set_current({});
			return;
		}

		set_current(it);
		sync_order();

		/* A local file may have disappeared since the last scan. Walk
		 * forward (wrapping) at most once around the whole list. */
		for (size_t tries = 0; tries < flat.size() && !playable(current()); tries++) {
			if (in_range(current()))
				blog(LOG_WARNING, LOG_PREFIX "skipping missing file '%s'",
				     path_of(current()).c_str());
			Item next;
			if (!step(+1, true, next))
				break;
			set_current(next);
		}

		const Item cur = current();
		if (!playable(cur)) {
			blog(LOG_WARNING, LOG_PREFIX "no playable item in the playlist");
			clear_child();
			set_current({});
			return;
		}

		const bool url = entries[cur.entry].is_url;
		const std::string path = path_of(cur);
		blog(LOG_INFO, LOG_PREFIX "loading '%s' (start %lld ms%s)", path.c_str(), (long long)start_ms,
		     (paused || by_hide) ? ", paused" : "");

		/* Signals that arrived before this point belong to the old media. */
		sig_started = false;
		sig_ended = false;
		self_stopped = false;
		stopped_by_hide = false;
		paused_by_hide = by_hide;
		pending_seek_ms = start_ms > 0 ? start_ms : 0;
		pending_pause = paused || by_hide;
		loading = true;
		load_start_ns = os_gettime_ns();

		const bool same_as_child = !url && ((!child_is_url && path == child_path) ||
						    (pushed_in_frame && !frame_base_url && path == frame_base_path));
		if (has_phase2 && phase2_path == path && phase2_url == url) {
			/* already waiting for the second phase */
		} else if (same_as_child) {
			/* The child ignores an unchanged path. Clear it now and
			 * set the real path one frame later (see video_tick). */
			push_child("", false);
			has_phase2 = true;
			phase2_path = path;
			phase2_url = false;
			phase2_due = frames_done + 1;
		} else {
			has_phase2 = false;
			push_child(path, url);
		}
		cur_dirty = true;
	}

	/* The current item finished: continue with the next one, or stop at the end. */
	void advance_after_end()
	{
		Item next;
		if (step(+1, loop, next)) {
			load(next, 0, false);
			return;
		}
		blog(LOG_INFO, LOG_PREFIX "end of playlist");
		obs_source_media_ended(source);
		cur_dirty = true;
	}

	/* -- resume ----------------------------------------------------------- */

	/* May be called from any thread. */
	bool build_resume_point(ResumePoint &rp)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (cur_entry < 0 || cur_file < 0 || (size_t)cur_entry >= entries.size() ||
			    (size_t)cur_file >= entries[cur_entry].files.size())
				return false;
			rp.entry = cur_entry;
			rp.file = cur_file;
			rp.path = entries[cur_entry].files[cur_file];
		}

		if (loading.load()) {
			rp.time_ms = pending_seek_ms.load();
			rp.paused = pending_pause.load() && !paused_by_hide.load();
		} else if (self_stopped.load()) {
			/* Stopped by the user: come back paused on the first frame.
			 * Stopped only because the source was hidden: just play again. */
			rp.time_ms = 0;
			rp.paused = !stopped_by_hide.load();
		} else {
			const obs_media_state st = obs_source_media_get_state(child);
			if (st == OBS_MEDIA_STATE_ENDED || st == OBS_MEDIA_STATE_STOPPED) {
				rp.time_ms = 0;
				rp.paused = true;
			} else {
				rp.time_ms = obs_source_media_get_time(child);
				rp.paused = st == OBS_MEDIA_STATE_PAUSED && !paused_by_hide.load();
			}
		}
		rp.saved_at = ResumeStore::now_ms();
		return true;
	}

	void save_resume_point()
	{
		ResumePoint rp;
		if (!build_resume_point(rp))
			return;
		ResumeStore::put(obs_source_get_uuid(source), rp);
		ResumeStore::flush();
	}

	/* -- update ----------------------------------------------------------- */

	/* Item to resume on startup, or an invalid one. */
	Item find_resume_item(const ResumePoint &rp) const
	{
		if (rp.path.empty())
			return {};
		const Item exact{rp.entry, rp.file};
		if (in_range(exact) && path_of(exact) == rp.path)
			return exact;
		return find_path(entries, rp.path, -1);
	}

	void update(obs_data_t *settings)
	{
		const bool first = first_update;
		first_update = false;

		const bool old_shuffle = shuffle;
		read_config(settings);
		const bool shuffle_changed = shuffle != old_shuffle;

		/* Remember what is loaded now, then swap in the new list. */
		const Item old_cur = current();
		const std::string old_path = in_range(old_cur) ? path_of(old_cur) : std::string();
		const int old_flat = flat_index(old_cur);

		std::set<std::string> rescan;
		{
			std::lock_guard<std::mutex> lock(mutex);
			rescan.swap(rescan_pending);
		}

		std::vector<EntryInfo> new_entries = build_entries(settings, folder_cache, rescan);
		std::vector<Item> new_flat = make_flat(new_entries);
		std::vector<std::string> new_paths;
		new_paths.reserve(new_flat.size());
		for (const Item &it : new_flat)
			new_paths.push_back(new_entries[it.entry].files[it.file]);

		Item new_cur;
		bool gone = false;
		if (!old_path.empty()) {
			/* Same path => keep playing it, even if its indices moved. */
			new_cur = find_path(new_entries, old_path, old_cur.entry);
			gone = !new_cur.valid();
		}

		const bool list_changed = new_paths != flat_paths;
		{
			/* entries and the current indices change together so that a
			 * snapshot never sees indices into the wrong list */
			std::lock_guard<std::mutex> lock(mutex);
			entries.swap(new_entries);
			cur_entry = new_cur.entry;
			cur_file = new_cur.file;
		}
		flat = std::move(new_flat);
		flat_paths = std::move(new_paths);
		playable_count = flat.size();
		if (new_cur != old_cur)
			cur_dirty = true;

		if (shuffle) {
			if (list_changed || shuffle_changed || order.size() != flat.size())
				rebuild_order(); /* current item first */
		} else {
			order.clear();
			order_pos = 0;
		}

		bool loaded = false;
		if (first) {
			loaded = load_initial(settings);
		} else if (old_cur.valid() && gone) {
			/* The playing file was removed from the list. */
			if (flat.empty()) {
				clear_child();
				cur_dirty = true;
			} else {
				const Item next = flat[(size_t)std::min(old_flat < 0 ? 0 : old_flat,
									(int)flat.size() - 1)];
				load(next, 0, false, hidden_now());
				loaded = true;
			}
		} else if (!old_cur.valid() && !flat.empty()) {
			load(first_item(), 0, false, hidden_now());
			loaded = true;
		}

		if (!loaded)
			sync_child_options();

		list_dirty = true;
	}

	/* First update (from create): resume the saved position or start at the top. */
	bool load_initial(obs_data_t *settings)
	{
		if (flat.empty())
			return false;

		const bool hide_pause = hidden_now();

		ResumePoint rp;
		bool have = false;
		if (resume_on_startup) {
			rp.path = obs_data_get_string(settings, "resume_path");
			rp.entry = (int)obs_data_get_int(settings, "resume_entry");
			rp.file = (int)obs_data_get_int(settings, "resume_file");
			rp.time_ms = obs_data_get_int(settings, "resume_time_ms");
			rp.paused = obs_data_get_bool(settings, "resume_paused");
			rp.saved_at = obs_data_get_int(settings, "resume_saved_at");
			have = !rp.path.empty();

			/* The store is flushed more often than the settings are saved. */
			ResumePoint stored;
			if (ResumeStore::get(obs_source_get_uuid(source), stored) && stored.saved_at > rp.saved_at) {
				rp = stored;
				have = !rp.path.empty();
			}
		}

		if (have) {
			const Item target = find_resume_item(rp);
			if (target.valid()) {
				load(target, rp.time_ms, rp.paused, hide_pause && !rp.paused);
				return true;
			}
		}
		load(first_item(), 0, false, hide_pause);
		return true;
	}

	/* Pushes changed hw_decode / speed / looping to the child when no load
	 * took care of it. */
	void sync_child_options()
	{
		const bool reopen = hw_decode != child_hw || speed != child_speed;

		if (reopen && in_range(current()) && (!child_path.empty() || has_phase2)) {
			/* Changing hw_decode or speed makes the child reopen the media
			 * from the start. Remember the position and seek back after
			 * "media_started". */
			if (!loading.load()) {
				const obs_media_state st = obs_source_media_get_state(child);
				if (self_stopped.load() || st == OBS_MEDIA_STATE_STOPPED || st == OBS_MEDIA_STATE_ENDED) {
					pending_seek_ms = 0;
					pending_pause = true; /* show the first frame, stay stopped-ish */
				} else {
					pending_seek_ms = obs_source_media_get_time(child);
					pending_pause = st == OBS_MEDIA_STATE_PAUSED;
				}
				self_stopped = false;
				stopped_by_hide = false;
				sig_started = false;
				sig_ended = false;
				loading = true;
				load_start_ns = os_gettime_ns();
			}
			if (has_phase2) {
				/* the pending second phase pushes the new values itself */
			} else {
				push_child(child_path, child_is_url);
			}
		} else if (reopen || (child_looping != want_looping() && !child_is_url)) {
			/* Nothing to reopen. For URLs a push would restart the stream,
			 * and looping does not apply to them anyway. */
			if (!child_is_url || child_path.empty())
				push_child(child_path, child_is_url);
			else {
				child_hw = hw_decode;
				child_speed = speed;
			}
		} else {
			child_looping = want_looping();
		}
	}

	/* -- child signals, processed in video_tick --------------------------- */

	void on_started()
	{
		if (loading.load()) {
			loading = false;
			const int64_t ms = pending_seek_ms.exchange(0);
			if (ms > 0)
				obs_source_media_set_time(child, ms);
			if (pending_pause.exchange(false))
				obs_source_media_play_pause(child, true);
			obs_source_media_started(source);
		}
		cur_dirty = true;
	}

	void on_ended()
	{
		/* Ignore the "ended" that the child emits while it replaces its
		 * media, the ones caused by our own stop, and stale ones. */
		if (loading.load() || self_stopped.load() || !current().valid())
			return;
		if (obs_source_media_get_state(child) != OBS_MEDIA_STATE_ENDED)
			return;
		advance_after_end();
	}

	/* -- video_tick ------------------------------------------------------- */

	/* Outputs the queued child audio as the playlist source's own audio. */
	void flush_audio()
	{
		std::deque<AudioChunk> chunks;
		{
			std::lock_guard<std::mutex> lock(audio_mutex);
			chunks.swap(audio_queue);
		}
		if (chunks.empty())
			return;

		struct obs_audio_info oai;
		if (!obs_get_audio_info(&oai))
			return;

		for (const AudioChunk &c : chunks) {
			struct obs_source_audio out = {};
			out.format = AUDIO_FORMAT_FLOAT_PLANAR;
			out.speakers = oai.speakers;
			out.samples_per_sec = oai.samples_per_sec;
			out.frames = c.frames;
			out.timestamp = c.timestamp;
			for (size_t i = 0; i < MAX_AV_PLANES && !c.planes[i].empty(); i++)
				out.data[i] = reinterpret_cast<const uint8_t *>(c.planes[i].data());
			obs_source_output_audio(source, &out);
		}
	}

	void tick()
	{
		flush_audio();

		/* Second phase of a same-path reload, one frame after the first. */
		if (has_phase2 && frames_done >= phase2_due) {
			has_phase2 = false;
			push_child(phase2_path, phase2_url);
		}

		if (sig_started.exchange(false))
			on_started();
		if (sig_ended.exchange(false))
			on_ended();
		if (sig_state.exchange(false))
			cur_dirty = true;

		/* Request from play_item() on the UI thread. */
		{
			Item req;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (has_request) {
					has_request = false;
					req = {req_entry, req_file};
				}
			}
			if (req.valid() && in_range(req))
				load(req, 0, false);
		}

		/* A local file that never starts (corrupt, unreadable): move on. */
		if (loading.load() && !child_is_url && !has_phase2 &&
		    os_gettime_ns() - load_start_ns > kLoadTimeoutNs) {
			blog(LOG_WARNING, LOG_PREFIX "timed out opening '%s'", child_path.c_str());
			loading = false;
			pending_seek_ms = 0;
			pending_pause = false;
			advance_after_end();
		}

		const uint64_t now = os_gettime_ns();
		if (now - last_resume_ns >= kResumeIntervalNs) {
			last_resume_ns = now;
			if (in_range(current()))
				save_resume_point();
		}

		flush_signals();

		pushed_in_frame = false;
		frames_done++;
	}

	/* -- visibility ------------------------------------------------------- */

	void activate()
	{
		if (paused_by_hide.exchange(false)) {
			if (loading.load())
				pending_pause = false; /* still opening: just start playing */
			else if (in_range(current()) && !child_path.empty() && !self_stopped.load())
				obs_source_media_play_pause(child, false);
		} else if (stopped_by_hide.exchange(false)) {
			if (in_range(current()))
				load(current(), 0, false);
		}
	}

	void deactivate()
	{
		if (visibility == kAlwaysPlay || !in_range(current()) || self_stopped.load())
			return;

		if (visibility == kPauseUnpause) {
			if (paused_by_hide.load())
				return;
			if (loading.load()) {
				if (!pending_pause.load()) {
					pending_pause = true;
					paused_by_hide = true;
				}
				return;
			}
			const obs_media_state st = obs_source_media_get_state(child);
			if (st == OBS_MEDIA_STATE_PLAYING || st == OBS_MEDIA_STATE_BUFFERING ||
			    st == OBS_MEDIA_STATE_OPENING) {
				obs_source_media_play_pause(child, true);
				paused_by_hide = true;
			}
			return;
		}

		/* kStopRestart */
		const obs_media_state st = obs_source_media_get_state(child);
		const bool active = loading.load() || st == OBS_MEDIA_STATE_PLAYING ||
				    st == OBS_MEDIA_STATE_PAUSED || st == OBS_MEDIA_STATE_BUFFERING ||
				    st == OBS_MEDIA_STATE_OPENING;
		if (!active)
			return;
		self_stopped = true;
		stopped_by_hide = true;
		paused_by_hide = false;
		loading = false;
		pending_seek_ms = 0;
		pending_pause = false;
		obs_source_media_stop(child);
	}

	/* -- media controls (graphics thread) --------------------------------- */

	void media_play_pause(bool pause)
	{
		if (flat.empty())
			return;

		paused_by_hide = false;
		stopped_by_hide = false;

		if (loading.load()) {
			pending_pause = pause;
			return;
		}

		const obs_media_state st = obs_source_media_get_state(child);
		const bool idle = st == OBS_MEDIA_STATE_ENDED || st == OBS_MEDIA_STATE_STOPPED ||
				  st == OBS_MEDIA_STATE_NONE || !in_range(current()) || child_path.empty();
		if (idle) {
			if (!pause)
				load(in_range(current()) ? current() : first_item(), 0, false);
			return;
		}
		obs_source_media_play_pause(child, pause);
	}

	void media_restart()
	{
		if (flat.empty())
			return;
		load(in_range(current()) ? current() : first_item(), 0, false);
	}

	void media_stop()
	{
		if (!in_range(current()))
			return;
		self_stopped = true;
		stopped_by_hide = false;
		paused_by_hide = false;
		loading = false;
		pending_seek_ms = 0;
		pending_pause = false;
		obs_source_media_stop(child);
		cur_dirty = true;
	}

	void media_step(int dir)
	{
		Item next;
		if (step(dir, loop, next))
			load(next, 0, false);
	}
};

PlaylistSource *from_source(obs_source_t *source)
{
	if (!source || !is_playlist_source(source))
		return nullptr;
	return static_cast<PlaylistSource *>(obs_obj_get_data(source));
}

/* ------------------------------------------------------------------------- */
/* Child signal callbacks: set flags only, real work happens in video_tick.   */
/* ------------------------------------------------------------------------- */

void on_child_started(void *param, calldata_t *)
{
	auto *ps = static_cast<PlaylistSource *>(param);
	ps->sig_started = true;
}

void on_child_ended(void *param, calldata_t *)
{
	auto *ps = static_cast<PlaylistSource *>(param);
	ps->sig_ended = true;
}

void on_child_state(void *param, calldata_t *)
{
	auto *ps = static_cast<PlaylistSource *>(param);
	ps->sig_state = true;
}

/* Copies the child's audio for video_tick. Runs with the child's filter_mutex
 * held, so it must not call into the parent source (see the file comment). */
void on_child_audio(void *param, obs_source_t *, const struct audio_data *data, bool)
{
	auto *ps = static_cast<PlaylistSource *>(param);
	audio_t *audio = obs_get_audio();
	if (!audio || !data->frames)
		return;

	AudioChunk chunk;
	chunk.frames = data->frames;
	chunk.timestamp = data->timestamp;
	const size_t channels = std::min<size_t>(audio_output_get_channels(audio), MAX_AV_PLANES);
	for (size_t i = 0; i < channels; i++) {
		if (!data->data[i])
			break;
		const float *in = reinterpret_cast<const float *>(data->data[i]);
		chunk.planes[i].assign(in, in + data->frames);
	}

	std::lock_guard<std::mutex> lock(ps->audio_mutex);
	if (ps->audio_queue.size() >= kMaxQueuedAudio)
		ps->audio_queue.pop_front();
	ps->audio_queue.push_back(std::move(chunk));
}

/* ------------------------------------------------------------------------- */
/* Hotkeys                                                                    */
/* ------------------------------------------------------------------------- */

void hotkey_pressed(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *ctx = static_cast<HotkeyCtx *>(data);
	obs_source_t *src = ctx->ps->source;

	switch (ctx->action) {
	case kHkPlayPause: {
		const obs_media_state st = ctx->ps->state();
		const bool playing = st == OBS_MEDIA_STATE_PLAYING || st == OBS_MEDIA_STATE_OPENING ||
				     st == OBS_MEDIA_STATE_BUFFERING;
		obs_source_media_play_pause(src, playing);
		break;
	}
	case kHkRestart:
		obs_source_media_restart(src);
		break;
	case kHkStop:
		obs_source_media_stop(src);
		break;
	case kHkNext:
		obs_source_media_next(src);
		break;
	case kHkPrevious:
		obs_source_media_previous(src);
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* obs_source_info callbacks                                                  */
/* ------------------------------------------------------------------------- */

const char *ps_get_name(void *)
{
	return tr("PlaylistSource");
}

void *ps_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ps = new PlaylistSource(source);
	ps->read_config(settings);
	ps->child_hw = ps->hw_decode;
	ps->child_speed = ps->speed;

	/* Private Media Source that does the actual playback. It never starts on
	 * its own (no restart_on_activate / close_when_inactive) and keeps its
	 * settings until we change them. */
	obs_data_t *cs = obs_data_create();
	obs_data_set_bool(cs, "is_local_file", true);
	obs_data_set_string(cs, "local_file", "");
	obs_data_set_bool(cs, "looping", false);
	obs_data_set_bool(cs, "restart_on_activate", false);
	obs_data_set_bool(cs, "close_when_inactive", false);
	obs_data_set_bool(cs, "clear_on_media_end", true);
	obs_data_set_bool(cs, "log_changes", false);
	obs_data_set_bool(cs, "hw_decode", ps->hw_decode);
	obs_data_set_int(cs, "speed_percent", ps->speed);
	ps->child = obs_source_create_private(kChildId, kChildName, cs);
	obs_data_release(cs);

	if (!ps->child) {
		blog(LOG_ERROR, LOG_PREFIX "could not create the internal media source (%s)", kChildId);
		delete ps;
		return nullptr;
	}

	obs_source_add_active_child(source, ps->child);

	signal_handler_t *csh = obs_source_get_signal_handler(ps->child);
	signal_handler_connect(csh, "media_started", on_child_started, ps);
	signal_handler_connect(csh, "media_ended", on_child_ended, ps);
	signal_handler_connect(csh, "media_pause", on_child_state, ps);
	signal_handler_connect(csh, "media_play", on_child_state, ps);
	signal_handler_connect(csh, "media_stopped", on_child_state, ps);
	obs_source_add_audio_capture_callback(ps->child, on_child_audio, ps);

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	signal_handler_add(sh, "void playlist_changed(ptr source)");
	signal_handler_add(sh, "void current_changed(ptr source)");

	static const struct {
		const char *name;
		const char *text;
	} hk[kHkCount] = {
		{"Gong2PlaylistSource.PlayPause", "Hotkey.PlayPause"},
		{"Gong2PlaylistSource.Restart", "Hotkey.Restart"},
		{"Gong2PlaylistSource.Stop", "Hotkey.Stop"},
		{"Gong2PlaylistSource.Next", "Hotkey.Next"},
		{"Gong2PlaylistSource.Previous", "Hotkey.Previous"},
	};
	for (int i = 0; i < kHkCount; i++) {
		ps->hotkeys[i].ps = ps;
		ps->hotkeys[i].action = i;
		obs_hotkey_register_source(source, hk[i].name, tr(hk[i].text), hotkey_pressed, &ps->hotkeys[i]);
	}

	ps->update(settings);
	return ps;
}

void ps_destroy(void *data)
{
	auto *ps = static_cast<PlaylistSource *>(data);

	if (ps->child) {
		signal_handler_t *csh = obs_source_get_signal_handler(ps->child);
		signal_handler_disconnect(csh, "media_started", on_child_started, ps);
		signal_handler_disconnect(csh, "media_ended", on_child_ended, ps);
		signal_handler_disconnect(csh, "media_pause", on_child_state, ps);
		signal_handler_disconnect(csh, "media_play", on_child_state, ps);
		signal_handler_disconnect(csh, "media_stopped", on_child_state, ps);
		obs_source_remove_audio_capture_callback(ps->child, on_child_audio, ps);
		obs_source_remove_active_child(ps->source, ps->child);
		obs_source_release(ps->child);
	}
	delete ps;
}

void ps_update(void *data, obs_data_t *settings)
{
	static_cast<PlaylistSource *>(data)->update(settings);
}

void ps_save(void *data, obs_data_t *settings)
{
	auto *ps = static_cast<PlaylistSource *>(data);

	/* now_playing only exists to feed the properties window */
	obs_data_erase(settings, "now_playing");

	ResumePoint rp;
	if (ps->build_resume_point(rp)) {
		obs_data_set_string(settings, "resume_path", rp.path.c_str());
		obs_data_set_int(settings, "resume_entry", rp.entry);
		obs_data_set_int(settings, "resume_file", rp.file);
		obs_data_set_int(settings, "resume_time_ms", rp.time_ms);
		obs_data_set_bool(settings, "resume_paused", rp.paused);
		obs_data_set_int(settings, "resume_saved_at", rp.saved_at);

		ResumeStore::put(obs_source_get_uuid(ps->source), rp);
		ResumeStore::flush();
	} else {
		obs_data_set_string(settings, "resume_path", "");
	}
}

void ps_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "loop", true);
	obs_data_set_default_bool(settings, "shuffle", false);
	obs_data_set_default_int(settings, "visibility_behavior", kPauseUnpause);
	obs_data_set_default_bool(settings, "resume_on_startup", true);
	obs_data_set_default_bool(settings, "hw_decode", false);
	obs_data_set_default_int(settings, "speed_percent", 100);
}

bool open_window_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ps = static_cast<PlaylistSource *>(data);
	OpenWindowCallback cb = g_open_window_cb.load();
	if (cb && ps)
		cb(ps->source);
	return false;
}

void append_patterns(const char *const *exts, size_t count, std::string &out)
{
	for (size_t i = 0; i < count; i++) {
		if (!out.empty())
			out += ' ';
		out += "*.";
		out += exts[i];
	}
}

obs_properties_t *ps_get_properties(void *data)
{
	auto *ps = static_cast<PlaylistSource *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_button2(props, "open_window", tr("OpenPlaylistWindow"), open_window_clicked, ps);

	std::string video, audio, all;
	append_patterns(kVideoExts, sizeof(kVideoExts) / sizeof(kVideoExts[0]), video);
	append_patterns(kAudioExts, sizeof(kAudioExts) / sizeof(kAudioExts[0]), audio);
	all = video + " " + audio;
	const std::string filter = std::string(tr("MediaFilter.AllMedia")) + " (" + all + ");;" +
				   tr("MediaFilter.Video") + " (" + video + ");;" + tr("MediaFilter.Audio") + " (" +
				   audio + ");;" + tr("MediaFilter.All") + " (*.*)";

	/* Start file dialogs next to the first local entry. */
	std::string default_dir;
	std::string np = tr("NothingPlaying");
	if (ps) {
		{
			std::lock_guard<std::mutex> lock(ps->mutex);
			for (const EntryInfo &e : ps->entries) {
				if (e.is_url)
					continue;
				const fs::path p = fs::u8path(e.path);
				default_dir = e.is_folder ? e.path : p.parent_path().u8string();
				break;
			}
		}
		np = ps->now_playing_text();
	}

	obs_properties_add_editable_list(props, "playlist", tr("Playlist"), OBS_EDITABLE_LIST_TYPE_FILES_AND_URLS,
					 filter.c_str(), default_dir.c_str());
	obs_properties_add_bool(props, "loop", tr("Loop"));
	obs_properties_add_bool(props, "shuffle", tr("Shuffle"));

	obs_property_t *p = obs_properties_add_list(props, "visibility_behavior", tr("VisibilityBehavior"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, tr("VisibilityBehavior.AlwaysPlay"), kAlwaysPlay);
	obs_property_list_add_int(p, tr("VisibilityBehavior.PauseUnpause"), kPauseUnpause);
	obs_property_list_add_int(p, tr("VisibilityBehavior.StopRestart"), kStopRestart);

	obs_properties_add_bool(props, "resume_on_startup", tr("ResumeOnStartup"));
	obs_properties_add_bool(props, "hw_decode", tr("HardwareDecode"));

	p = obs_properties_add_int_slider(props, "speed_percent", tr("SpeedPercent"), 1, 200, 1);
	obs_property_int_set_suffix(p, "%");

	/* The properties view shows the setting's value as the info text and the
	 * long description as its tooltip; fill in both. The value is dropped
	 * again in ps_save. */
	p = obs_properties_add_text(props, "now_playing", tr("NowPlaying"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(p, true);
	obs_property_set_long_description(p, np.c_str());
	if (ps) {
		obs_data_t *settings = obs_source_get_settings(ps->source);
		obs_data_set_string(settings, "now_playing", np.c_str());
		obs_data_release(settings);
	}

	return props;
}

void ps_activate(void *data)
{
	static_cast<PlaylistSource *>(data)->activate();
}

void ps_deactivate(void *data)
{
	static_cast<PlaylistSource *>(data)->deactivate();
}

void ps_video_tick(void *data, float)
{
	static_cast<PlaylistSource *>(data)->tick();
}

void ps_video_render(void *data, gs_effect_t *)
{
	auto *ps = static_cast<PlaylistSource *>(data);
	if (ps->child && ps->playable_count.load() > 0)
		obs_source_video_render(ps->child);
}

uint32_t ps_get_width(void *data)
{
	auto *ps = static_cast<PlaylistSource *>(data);
	return ps->child ? obs_source_get_width(ps->child) : 0;
}

uint32_t ps_get_height(void *data)
{
	auto *ps = static_cast<PlaylistSource *>(data);
	return ps->child ? obs_source_get_height(ps->child) : 0;
}

void ps_enum_active_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	auto *ps = static_cast<PlaylistSource *>(data);
	if (ps->child)
		cb(ps->source, ps->child, param);
}

enum gs_color_space ps_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	auto *ps = static_cast<PlaylistSource *>(data);
	if (!ps->child)
		return GS_CS_SRGB;
	return obs_source_get_color_space(ps->child, count, preferred_spaces);
}

/* Media controls: libobs queues these and runs them in our video tick. */

void ps_media_play_pause(void *data, bool pause)
{
	static_cast<PlaylistSource *>(data)->media_play_pause(pause);
}

void ps_media_restart(void *data)
{
	static_cast<PlaylistSource *>(data)->media_restart();
}

void ps_media_stop(void *data)
{
	static_cast<PlaylistSource *>(data)->media_stop();
}

void ps_media_next(void *data)
{
	static_cast<PlaylistSource *>(data)->media_step(+1);
}

void ps_media_previous(void *data)
{
	static_cast<PlaylistSource *>(data)->media_step(-1);
}

int64_t ps_media_get_duration(void *data)
{
	auto *ps = static_cast<PlaylistSource *>(data);
	return ps->child ? obs_source_media_get_duration(ps->child) : 0;
}

int64_t ps_media_get_time(void *data)
{
	auto *ps = static_cast<PlaylistSource *>(data);
	if (ps->loading.load())
		return ps->pending_seek_ms.load();
	return ps->child ? obs_source_media_get_time(ps->child) : 0;
}

void ps_media_set_time(void *data, int64_t ms)
{
	auto *ps = static_cast<PlaylistSource *>(data);
	if (ps->loading.load())
		ps->pending_seek_ms = ms; /* applied when the child reports "media_started" */
	else if (ps->child)
		obs_source_media_set_time(ps->child, ms);
}

enum obs_media_state ps_media_get_state(void *data)
{
	return static_cast<PlaylistSource *>(data)->state();
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

void register_source()
{
	static struct obs_source_info info = {};
	info.id = PLAYLIST_SOURCE_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags =
		OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_AUDIO | OBS_SOURCE_CONTROLLABLE_MEDIA;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	info.get_name = ps_get_name;
	info.create = ps_create;
	info.destroy = ps_destroy;
	info.update = ps_update;
	info.save = ps_save;
	info.get_defaults = ps_get_defaults;
	info.get_properties = ps_get_properties;
	info.activate = ps_activate;
	info.deactivate = ps_deactivate;
	info.video_tick = ps_video_tick;
	info.video_render = ps_video_render;
	info.get_width = ps_get_width;
	info.get_height = ps_get_height;
	info.enum_active_sources = ps_enum_active_sources;
	info.video_get_color_space = ps_get_color_space;
	info.media_play_pause = ps_media_play_pause;
	info.media_restart = ps_media_restart;
	info.media_stop = ps_media_stop;
	info.media_next = ps_media_next;
	info.media_previous = ps_media_previous;
	info.media_get_duration = ps_media_get_duration;
	info.media_get_time = ps_media_get_time;
	info.media_set_time = ps_media_set_time;
	info.media_get_state = ps_media_get_state;
	obs_register_source(&info);

	static struct obs_source_info legacy = info;
	legacy.id = PLAYLIST_SOURCE_LEGACY_ID;
	legacy.output_flags |= OBS_SOURCE_CAP_DISABLED;
	obs_register_source(&legacy);
}

void shutdown()
{
	ResumeStore::flush();
}

bool is_playlist_source(obs_source_t *source)
{
	if (!source)
		return false;
	const char *id = obs_source_get_unversioned_id(source);
	return id && (strcmp(id, PLAYLIST_SOURCE_ID) == 0 || strcmp(id, PLAYLIST_SOURCE_LEGACY_ID) == 0);
}

bool get_snapshot(obs_source_t *source, Snapshot &out)
{
	PlaylistSource *ps = from_source(source);
	if (!ps)
		return false;

	{
		std::lock_guard<std::mutex> lock(ps->mutex);
		out.entries = ps->entries;
		out.current_entry = ps->cur_entry;
		out.current_file = ps->cur_file;
	}
	out.state = ps->state();
	return true;
}

void play_item(obs_source_t *source, int entry, int file)
{
	PlaylistSource *ps = from_source(source);
	if (!ps)
		return;

	std::lock_guard<std::mutex> lock(ps->mutex);
	if (entry < 0 || file < 0 || (size_t)entry >= ps->entries.size() ||
	    (size_t)file >= ps->entries[entry].files.size())
		return;
	ps->has_request = true;
	ps->req_entry = entry;
	ps->req_file = file;
}

void set_entries(obs_source_t *source, const std::vector<std::string> &paths)
{
	if (!is_playlist_source(source))
		return;

	obs_data_array_t *array = obs_data_array_create();
	for (const std::string &path : paths) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "value", path.c_str());
		obs_data_set_bool(item, "selected", false);
		obs_data_set_bool(item, "hidden", false);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_array(settings, "playlist", array);
	obs_data_array_release(array);

	/* Drop the saved orders and removed files of folders that are no longer in the list. */
	obs_data_t *current = obs_source_get_settings(source);
	std::vector<FolderState> states = read_folder_states(current);
	obs_data_release(current);
	const size_t before = states.size();
	states.erase(std::remove_if(states.begin(), states.end(),
				    [&](const FolderState &st) {
					    return std::find(paths.begin(), paths.end(), st.folder) == paths.end();
				    }),
		     states.end());
	if (states.size() != before) {
		obs_data_array_t *pruned = make_folder_states_array(states);
		obs_data_set_array(settings, "folder_orders", pruned);
		obs_data_array_release(pruned);
	}

	obs_source_update(source, settings);
	obs_data_release(settings);
}

namespace {

/* Applies `edit` to the saved state of `folder` (created if missing, dropped
 * when it ends up with neither an order nor removed files) and writes the
 * "folder_orders" setting. `edit` returns false for "no change". */
template<typename Fn> void edit_folder_state(obs_source_t *source, const std::string &folder, Fn edit)
{
	obs_data_t *current = obs_source_get_settings(source);
	std::vector<FolderState> states = read_folder_states(current);
	obs_data_release(current);

	auto it = std::find_if(states.begin(), states.end(), [&](const FolderState &st) { return st.folder == folder; });
	FolderState st;
	if (it != states.end())
		st = *it;
	st.folder = folder;
	if (!edit(st))
		return;

	if (it != states.end())
		states.erase(it);
	states.push_back(std::move(st));

	obs_data_array_t *array = make_folder_states_array(states);
	obs_data_t *settings = obs_data_create();
	obs_data_set_array(settings, "folder_orders", array);
	obs_data_array_release(array);

	obs_source_update(source, settings);
	obs_data_release(settings);
}

} // namespace

void set_folder_order(obs_source_t *source, const std::string &folder, const std::vector<std::string> &files)
{
	if (!is_playlist_source(source))
		return;

	edit_folder_state(source, folder, [&](FolderState &st) {
		st.names.clear();
		for (const std::string &f : files)
			st.names.push_back(file_name_of(f));
		return true;
	});
}

void exclude_folder_files(obs_source_t *source, const std::string &folder, const std::vector<std::string> &files)
{
	if (!is_playlist_source(source) || files.empty())
		return;

	edit_folder_state(source, folder, [&](FolderState &st) {
		for (const std::string &f : files) {
			const std::string name = file_name_of(f);
			if (std::find(st.excluded.begin(), st.excluded.end(), name) == st.excluded.end())
				st.excluded.push_back(name);
		}
		return true;
	});
}

void restore_folder_files(obs_source_t *source, const std::string &folder)
{
	if (!is_playlist_source(source))
		return;

	edit_folder_state(source, folder, [](FolderState &st) {
		if (st.excluded.empty())
			return false;
		st.excluded.clear();
		return true;
	});
}

void rescan_folder(obs_source_t *source, const std::string &folder)
{
	PlaylistSource *ps = from_source(source);
	if (!ps)
		return;

	{
		std::lock_guard<std::mutex> lock(ps->mutex);
		ps->rescan_pending.insert(folder);
	}
	obs_source_update(source, nullptr); /* update() runs on the graphics thread and takes the request */
}

void set_open_window_callback(OpenWindowCallback cb)
{
	g_open_window_cb.store(cb);
}

} // namespace playlist
