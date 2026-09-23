#!/bin/sh
# Usage: tests/host/face_preview.sh [out.png] [mouth-open 0..1] [ms]
set -eu
cd "$(dirname "$0")/../.."
out=${1:-/tmp/bunty-faces.png}
bin=$(mktemp -d /tmp/bunty-face.XXXXXX)/face_preview
"${CXX:-c++}" -std=c++17 -O2 -Itests/host/stubs -Isrc \
  tests/host/face_preview.cpp src/Face.cpp src/flow32/graphics/Display.cpp -o "$bin"
"$bin" "${out%.png}.ppm" "${2:-0.5}" "${3:-700}"
sips -s format png "${out%.png}.ppm" --out "$out" >/dev/null
echo "$out"
