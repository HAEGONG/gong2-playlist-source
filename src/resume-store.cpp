/*
gong2 Playlist Source for OBS
Copyright (C) 2026

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "resume-store.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <chrono>
#include <map>
#include <mutex>

namespace playlist {

namespace {

/* Points older than this are dropped when the file is loaded. */
constexpr int64_t kMaxAgeMs = 30LL * 24 * 60 * 60 * 1000;

std::mutex g_mutex;    /* guards the members below */
std::mutex g_io_mutex; /* serializes file writes; always taken before g_mutex */
std::map<std::string, ResumePoint> g_points;
bool g_loaded = false;
bool g_dirty = false;

std::string store_path()
{
	char *p = obs_module_config_path("resume.json");
	std::string path = p ? p : "";
	bfree(p);
	return path;
}

/* Caller holds g_mutex. */
void ensure_loaded()
{
	if (g_loaded)
		return;
	g_loaded = true;

	const std::string path = store_path();
	if (path.empty())
		return;

	obs_data_t *root = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!root)
		return;

	obs_data_t *sources = obs_data_get_obj(root, "sources");
	if (sources) {
		const int64_t now = ResumeStore::now_ms();
		for (obs_data_item_t *item = obs_data_first(sources); item; obs_data_item_next(&item)) {
			const char *uuid = obs_data_item_get_name(item);
			obs_data_t *o = obs_data_item_get_obj(item);
			if (!uuid || !o) {
				obs_data_release(o);
				continue;
			}

			ResumePoint p;
			p.path = obs_data_get_string(o, "path");
			p.entry = (int)obs_data_get_int(o, "entry");
			p.file = (int)obs_data_get_int(o, "file");
			p.time_ms = obs_data_get_int(o, "time_ms");
			p.paused = obs_data_get_bool(o, "paused");
			p.saved_at = obs_data_get_int(o, "saved_at");
			obs_data_release(o);

			if (p.path.empty() || now - p.saved_at > kMaxAgeMs) {
				g_dirty = true; /* rewrite the file without the stale point */
				continue;
			}
			g_points[uuid] = p;
		}
		obs_data_release(sources);
	}
	obs_data_release(root);
}

} // namespace

int64_t ResumeStore::now_ms()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool ResumeStore::get(const char *uuid, ResumePoint &out)
{
	if (!uuid || !*uuid)
		return false;

	std::lock_guard<std::mutex> lock(g_mutex);
	ensure_loaded();
	auto it = g_points.find(uuid);
	if (it == g_points.end())
		return false;
	out = it->second;
	return true;
}

void ResumeStore::put(const char *uuid, const ResumePoint &p)
{
	if (!uuid || !*uuid)
		return;

	std::lock_guard<std::mutex> lock(g_mutex);
	ensure_loaded();

	auto it = g_points.find(uuid);
	if (it != g_points.end()) {
		const ResumePoint &old = it->second;
		/* saved_at alone changes on every call; it is not worth a disk write */
		const bool same = old.path == p.path && old.entry == p.entry && old.file == p.file &&
				  old.time_ms == p.time_ms && old.paused == p.paused;
		it->second = p;
		if (!same)
			g_dirty = true;
		return;
	}

	g_points[uuid] = p;
	g_dirty = true;
}

void ResumeStore::flush()
{
	std::lock_guard<std::mutex> io_lock(g_io_mutex);

	const std::string path = store_path();
	obs_data_t *root = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		ensure_loaded();
		if (!g_dirty || path.empty())
			return;

		root = obs_data_create();
		obs_data_t *sources = obs_data_create();
		for (const auto &kv : g_points) {
			const ResumePoint &p = kv.second;
			obs_data_t *o = obs_data_create();
			obs_data_set_string(o, "path", p.path.c_str());
			obs_data_set_int(o, "entry", p.entry);
			obs_data_set_int(o, "file", p.file);
			obs_data_set_int(o, "time_ms", p.time_ms);
			obs_data_set_bool(o, "paused", p.paused);
			obs_data_set_int(o, "saved_at", p.saved_at);
			obs_data_set_obj(sources, kv.first.c_str(), o);
			obs_data_release(o);
		}
		obs_data_set_obj(root, "sources", sources);
		obs_data_release(sources);
		g_dirty = false;
	}

	/* The disk write happens outside g_mutex so put()/get() never wait for I/O. */
	const size_t slash = path.find_last_of("/\\");
	if (slash != std::string::npos)
		os_mkdirs(path.substr(0, slash).c_str());

	if (!obs_data_save_json_safe(root, path.c_str(), "tmp", "bak")) {
		blog(LOG_WARNING, "[gong2-playlist-source] failed to write %s", path.c_str());
		std::lock_guard<std::mutex> lock(g_mutex);
		g_dirty = true; /* try again on the next flush */
	}
	obs_data_release(root);
}

} // namespace playlist
