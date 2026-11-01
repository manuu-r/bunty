#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
out=${1:-/tmp/bunty-shell-preview.png}
tmp=$(mktemp -d /tmp/bunty-shell.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I.deps/gfx/include -Itests/host/stubs -Ilib/bunty_platform -Isrc \
  tests/host/shell_preview.cpp src/ShellView.cpp src/Fonts.cpp .deps/gfx/Adafruit_GFX.cpp \
  -o "$tmp/preview"
"$tmp/preview" "$tmp/preview.ppm"
sips -s format png "$tmp/preview.ppm" --out "$out" >/dev/null
printf '%s\n' "$out"
