#!/bin/sh
# Usage: tests/host/face_preview.sh [out.png] [mouth-open 0..1] [ms]
set -eu
cd "$(dirname "$0")/../.."
out=${1:-/tmp/bunty-aura.png}
preview_dir=$(mktemp -d /tmp/bunty-face.XXXXXX)
trap 'rm -rf "$preview_dir"' EXIT
bin=$preview_dir/face_preview
"${CXX:-c++}" -std=c++17 -O2 -I.deps/gfx/include -Itests/host/stubs -Ilib/bunty_platform -Isrc \
  tests/host/aura_preview.cpp src/ShellView.cpp src/Fonts.cpp .deps/gfx/Adafruit_GFX.cpp src/flow32/graphics/Display.cpp -o "$bin"
"$bin" "$preview_dir/faces.ppm" 
sips -s format png "$preview_dir/faces.ppm" --out "$out" >/dev/null
echo "$out"
