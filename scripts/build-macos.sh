#!/bin/zsh
# Builds the plugin against the installed OBS.app and (with --install) copies it
# into ~/Library/Application Support/obs-studio/plugins.
#
#   scripts/build-macos.sh [--install] [--clean]
#
# Dependencies are fetched once into .deps/:
#   - obs-studio source tree (headers only) matching the installed OBS version
#   - obs-deps prebuilt Qt 6 (same Qt build OBS ships with)
#   - obs-deps prebuilt dependencies (only the simde headers are used)

set -euo pipefail

ROOT=${0:A:h:h}
DEPS="$ROOT/.deps"
BUILD="$ROOT/build_macos"
OBS_APP=${OBS_APP:-/Applications/OBS.app}

OBS_VERSION=${OBS_VERSION:-$(defaults read "$OBS_APP/Contents/Info.plist" CFBundleShortVersionString)}
# obs-deps release used by OBS 33.0.x (see obs-studio CMakePresets.json)
OBS_DEPS_VERSION=${OBS_DEPS_VERSION:-2026-10-23}
ARCH=$(uname -m)

install=0
for arg in "$@"; do
  case $arg in
    --install) install=1 ;;
    --clean) rm -rf "$BUILD" ;;
    *) print -u2 "unknown option: $arg"; exit 1 ;;
  esac
done

mkdir -p "$DEPS"

if [[ ! -f "$DEPS/obs-studio/libobs/obs-module.h" ]]; then
  print "Fetching obs-studio $OBS_VERSION sources..."
  curl -fL -o "$DEPS/obs-studio.tar.gz" "https://github.com/obsproject/obs-studio/archive/refs/tags/$OBS_VERSION.tar.gz"
  rm -rf "$DEPS/obs-studio" && mkdir -p "$DEPS/obs-studio"
  tar -xzf "$DEPS/obs-studio.tar.gz" -C "$DEPS/obs-studio" --strip-components=1
fi

if [[ ! -d "$DEPS/qt6/lib/cmake/Qt6" ]]; then
  print "Fetching obs-deps Qt6 $OBS_DEPS_VERSION ($ARCH)..."
  curl -fL -o "$DEPS/qt6.tar.xz" \
    "https://github.com/obsproject/obs-deps/releases/download/$OBS_DEPS_VERSION/macos-deps-qt6-$OBS_DEPS_VERSION-$ARCH.tar.xz"
  rm -rf "$DEPS/qt6" && mkdir -p "$DEPS/qt6"
  tar -xJf "$DEPS/qt6.tar.xz" -C "$DEPS/qt6"
  # Downloaded binaries carry the quarantine flag, which blocks moc from running.
  xattr -r -d com.apple.quarantine "$DEPS/qt6" 2>/dev/null || true
fi

if [[ ! -d "$DEPS/obs-deps/include/simde" ]]; then
  print "Fetching obs-deps $OBS_DEPS_VERSION ($ARCH)..."
  curl -fL -o "$DEPS/obs-deps.tar.xz" \
    "https://github.com/obsproject/obs-deps/releases/download/$OBS_DEPS_VERSION/macos-deps-$OBS_DEPS_VERSION-$ARCH.tar.xz"
  rm -rf "$DEPS/obs-deps" && mkdir -p "$DEPS/obs-deps"
  tar -xJf "$DEPS/obs-deps.tar.xz" -C "$DEPS/obs-deps"
fi

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DGONG2_LOCAL_OBS_APP=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  -DCMAKE_PREFIX_PATH="$DEPS/qt6" \
  -DOBS_SOURCE_DIR="$DEPS/obs-studio" \
  -DOBS_DEPS_DIR="$DEPS/obs-deps" \
  -DOBS_APP="$OBS_APP"
cmake --build "$BUILD"

if (( install )); then
  DEST="$HOME/Library/Application Support/obs-studio/plugins"
  mkdir -p "$DEST"
  # Name used before the gong2 rename; both would register the same sources.
  rm -rf "$DEST/playlist-source.plugin"
  rm -rf "$DEST/gong2-playlist-source.plugin"
  cp -R "$BUILD/gong2-playlist-source.plugin" "$DEST/"
  print "Installed to $DEST/gong2-playlist-source.plugin"
fi
