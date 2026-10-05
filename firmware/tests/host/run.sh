#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
python3 scripts/prepare_deps.py
test_dir=$(mktemp -d /tmp/bunty-host.XXXXXX)
trap 'rm -rf "$test_dir"' EXIT
# Set BUNTY_SANITIZERS=address,undefined on hosts with a working ASan runtime.
# Apple's installed ASan currently deadlocks during dyld initialization here.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Itests/host/stubs -Isrc -I.deps/arduinojson/src \
  tests/host/memory_display_test.cpp src/flow32/graphics/Display.cpp src/InternalAllocation.cpp \
  -o "$test_dir/memory_display_test"
"$test_dir/memory_display_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Itests/host/stubs -Isrc tests/host/touch_test.cpp -o "$test_dir/touch_test"
"$test_dir/touch_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Isrc -I.deps/arduinojson/src tests/host/dashboard_cache_test.cpp -o "$test_dir/dashboard_cache_test"
"$test_dir/dashboard_cache_test"

# Exercise the actual cached graphics/Print adaptation, not the display stub.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -I.deps/gfx/include -Itests/host/stubs -Ilib/bunty_platform -Isrc \
  tests/host/native_gfx_test.cpp .deps/gfx/Adafruit_GFX.cpp src/Fonts.cpp \
  -o "$test_dir/native_gfx_test"
"$test_dir/native_gfx_test"

# Read fixtures using the same NVS representations as Arduino Preferences.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Itests/host/stubs -Ilib/bunty_platform \
  tests/host/native_storage_test.cpp lib/bunty_platform/Preferences.cpp \
  -o "$test_dir/native_storage_test"
"$test_dir/native_storage_test"
