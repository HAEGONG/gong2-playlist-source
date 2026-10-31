/*
SPDX-License-Identifier: 0BSD

gong2 Playlist Source for OBS
Copyright (C) 2026 HAEGONG

See LICENSE for permission to use, copy, modify, and distribute this software.
*/

#pragma once

/*
 * Persists the last playback position of every playlist source in
 * <plugin config dir>/resume.json, keyed by source uuid.
 *
 * The position is also stored in the source settings (see ps_save), but
 * settings are only written when OBS saves the scene collection, which does not
 * happen on a crash. The store is flushed periodically and therefore usually
 * holds a newer position; the source picks whichever has the newer saved_at.
 */

#include <cstdint>
#include <string>

namespace playlist {

struct ResumePoint {
	std::string path;
	int entry = -1;
	int file = -1;
	int64_t time_ms = 0;
	bool paused = false;
	int64_t saved_at = 0; /* wall-clock unix time in milliseconds */
};

class ResumeStore {
public:
	/* Thread-safe. Returns false if there is no point for this uuid. */
	static bool get(const char *uuid, ResumePoint &out);

	/* Thread-safe. Only updates memory; call flush() to write to disk. */
	static void put(const char *uuid, const ResumePoint &p);

	/* Thread-safe. Writes resume.json if anything changed since the last flush. */
	static void flush();

	/* Wall-clock unix time in milliseconds (the unit of ResumePoint::saved_at). */
	static int64_t now_ms();
};

} // namespace playlist
