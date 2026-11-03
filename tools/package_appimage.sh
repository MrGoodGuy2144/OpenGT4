#!/usr/bin/env bash
# Usage: tools/package_appimage.sh BUILD_DIR /path/to/linuxdeploy.AppImage /path/to/ffmpeg
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=$(realpath -- "${1:?build directory required}")
deploy=$(realpath -- "${2:?linuxdeploy executable required}")
ffmpeg=$(realpath -- "${3:?FFmpeg executable required}")
[[ $(uname -m) == x86_64 ]] || { echo 'Use an x86_64 build host.' >&2; exit 2; }
[[ -x $build/opengt4 && -x $deploy && -x $ffmpeg ]] || { echo 'Missing executable input.' >&2; exit 2; }
# Always stage a fresh directory; never package the checkout or game data.
mkdir -p "$root/dist"
stage=$(mktemp -d "$root/dist/appimage-stage.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
appdir=$stage/AppDir
mkdir -p "$appdir/usr/share/doc/opengt4"
cp "$root/LICENSE" "$root/NOTICE" "$appdir/usr/share/doc/opengt4/"
cp "$root/docs/STEAM_DECK.md" "$appdir/usr/share/doc/opengt4/"
# Record the exact bundled decoder, not an assumed LGPL/GPL configuration.
"$ffmpeg" -version > "$appdir/usr/share/doc/opengt4/ffmpeg-build.txt" 2>&1
for component in ps2float stb fsr1; do
    mkdir -p "$appdir/usr/share/doc/opengt4/$component"
    find "$root/third_party/$component" -maxdepth 1 -type f \( -iname '*license*' -o -iname '*copying*' \) -exec cp {} "$appdir/usr/share/doc/opengt4/$component/" \;
done
# SDL/miniz are statically linked by CMake. Ship their actual licence texts.
for entry in 'sdl2-src/LICENSE.txt' 'miniz-src/LICENSE'; do
    [[ -f $build/_deps/$entry ]] || { echo "Missing dependency licence: $entry" >&2; exit 2; }
    cp "$build/_deps/$entry" "$appdir/usr/share/doc/opengt4/$(dirname "$entry")-LICENSE.txt"
done
cat > "$appdir/usr/share/doc/opengt4/THIRD_PARTY.txt" <<'EOF'
Based on OpenGT4. This is a modified, unofficial experimental Linux build.
Source: https://github.com/omegakatana92/OpenGT4-steam-deck-edition
SDL2 (zlib), miniz (MIT), stb_image (MIT), FSR1 (MIT), ps2float (LGPL-3.0-or-later).
FFmpeg is a separate executable; see ffmpeg-build.txt for its build configuration.
Dependency libraries collected by linuxdeploy retain their respective licences.
Before publishing a release, include matching dependency copyright notices and
provide corresponding source for all bundled GPL/LGPL components, including the
exact FFmpeg package and its dynamic dependencies. This script creates a local
test artifact; it does not publish or certify a distribution-ready release.
EOF
cd "$root/dist"
export ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1
export OUTPUT=OpenGT4-Steam-Deck-experimental-x86_64.AppImage
"$deploy" --appdir "$appdir" --executable "$build/opengt4" --executable "$ffmpeg" \
    --desktop-file "$root/packaging/linux/opengt4-steam-deck.desktop" \
    --icon-file "$root/packaging/linux/opengt4-steam-deck.svg" \
    --custom-apprun "$root/packaging/linux/AppRun"
# linuxdeploy/patchelf can leave files with owner-only permissions. Normalize
# the completed staging tree before the output plugin reads it.
chmod -R a+rX "$appdir"
"$deploy" --appdir "$appdir" --output appimage
# Only these explicit inputs and their runtime libraries enter the image.
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
