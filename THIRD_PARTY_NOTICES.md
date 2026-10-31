# License scope and third-party notices

The [0BSD license](LICENSE) covers the original project code in `src/`,
`data/`, `scripts/build-macos.sh`, and `cmake/local-obs-app.cmake`, as well as
this project's documentation and logo in `assets/`. It permits use, modification, commercial use,
and redistribution without requiring attribution.

## OBS plugin build template

The build and packaging infrastructure is adapted from
[obsproject/obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
The template-derived files retain their GNU GPL version 2 license; they are
not relicensed under 0BSD. The license text is in
[LICENSES/GPL-2.0.txt](LICENSES/GPL-2.0.txt).

This applies to `.github/`, `cmake/` (except `cmake/local-obs-app.cmake`),
`CMakeLists.txt`, `CMakePresets.json`, `buildspec.json`, and `.gitignore`.
Existing upstream copyright and license notices remain applicable.

## OBS and other dependencies

The 0BSD grant covers the original project code only and does not change
the licenses of OBS Studio, Qt, FFmpeg, or other dependencies. In particular,
the plugin links to `libobs` and `obs-frontend-api`. Distribution of the
OBS-linked plugin must comply with the applicable GNU GPL terms, including
providing the corresponding source code. This is described in the
[OBS plugin distribution guide](https://github.com/obsproject/obs-plugintemplate/wiki/Distribute-Your-Plugin).

The 0BSD license does not waive these third-party requirements.
