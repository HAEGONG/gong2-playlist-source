English | [한국어](README.ko.md)

# gong2 Playlist Source for OBS

A playlist source for OBS that replaces the VLC Video Source. It uses OBS's built-in **Media Source (ffmpeg_source)** internally, so VLC is not required, and you keep the Media Source features such as hardware decoding, HDR, and speed control.

## Features

- **Add files / folders / URLs** — When you add a folder, the media files inside it play in file name order (natural sort: `ep2` < `ep10`). The folder contents are read when you add the folder (or when OBS starts); after that they are re-read only when you right-click the folder and choose *Rescan This Folder*.
- **Edit the list while playing** — Adding, removing, or reordering items does not interrupt the video that is currently playing. If the playing file is removed from the list, the next item at the same position plays.
- **Play any item you choose** — Double-click it in the playlist window.
- **Restore playback position when OBS restarts** — Remembers the file and playback position at shutdown (including the paused state) and resumes from there. Besides saving with the scene collection, the position is also written to `resume.json` every 5 seconds, so it survives an unexpected OBS crash.
- **Playlist window** — Tools → *gong2 Playlist Window*, or the *Open Playlist Window* button in the source properties. It is an OBS dock, so you can float it as a separate window or attach it to the main window.
  - The currently playing item is highlighted; double-click to play
  - Reorder by drag and drop, and drag files/folders in from Finder
  - Files inside a folder can also be reordered by drag and drop or with the Move Up / Move Down buttons. New files picked up by *Rescan This Folder* are added at the end in name order, and *Reset to Name Order* in the right-click menu resets the order.
  - Files inside a folder can also be selected and removed (Delete key / Remove button / right-click menu). They are only removed from the playlist, not deleted from disk, and *Restore Removed Files* in the right-click menu brings them back.
  - Previous / Play/Pause / Stop / Next, and a seek slider
- Loop, shuffle, behavior when the source is hidden (keep playing / pause / stop), and hotkeys (Play/Pause, Restart, Stop, Next, Previous)

## Installation

Download the installer for your platform from the [GitHub Releases](https://github.com/HAEGONG/gong2-playlist-source/releases) page:

- macOS: `.pkg` (universal)
- Windows: installer `.exe` or `.zip`
- Linux: `.deb`

Then restart OBS and use *Add Source → gong2 Playlist Source* and *Tools → gong2 Playlist Window*.

Supported: OBS 33 or later (built against OBS 33.0.0).

**macOS note:** The package is not notarized. If macOS blocks it, right-click the `.pkg` and choose *Open*, or allow it in System Settings → Privacy & Security.

## Build from source (macOS, without Xcode)

Release packages are built by GitHub Actions; see `.github/workflows`.

The following builds locally. It uses Command Line Tools + CMake + Ninja, with no Xcode. It links directly against the libraries of the installed `/Applications/OBS.app`, and takes headers from the obs-studio source of the same version.

```bash
brew install cmake ninja
```

```bash
scripts/build-macos.sh --install
```

On the first run it downloads the following into `.deps/`: the obs-studio source (the same tag as the installed OBS version), and obs-deps' Qt 6 and dependency packages (the same builds OBS ships with). `--install` copies the result to `~/Library/Application Support/obs-studio/plugins/gong2-playlist-source.plugin`. After restarting OBS, you will see *Add Source → gong2 Playlist Source*. (If the old-name `playlist-source.plugin` is installed, it is removed as well.)

If the OBS version changes, delete `.deps/obs-studio` and build again. You can change the obs-deps version with the `OBS_DEPS_VERSION` environment variable (see `CMakePresets.json` of that OBS version).

## Structure

| File | Role |
| --- | --- |
| `src/playlist-source.cpp` | The source itself. Creates and controls the internal media source and passes through video/audio; manages playback order, loop, shuffle, and playback position |
| `src/resume-store.cpp` | Writes the playback position to `resume.json` (in the plugin config folder) |
| `src/playlist-window.cpp` | The Qt playlist window (OBS dock) |
| `src/playlist-source.hpp` | The API between the source and the window |

The playlist is stored in the source settings' `playlist` entry (OBS editable-list format), so the properties dialog and the playlist window edit the same data.

## License

GPL-2.0-or-later. The structure and ideas are based on [CodeYan01/media-playlist-source](https://github.com/CodeYan01/media-playlist-source).
