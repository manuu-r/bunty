#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d /tmp/bunty-host.XXXXXX)
# Set BUNTY_SANITIZERS=address,undefined on hosts with a working ASan runtime.
# Apple's installed ASan currently deadlocks during dyld initialization here.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Itests/host/stubs -Isrc -I.pio/libdeps/bunty/ArduinoJson/src \
  tests/host/memory_display_test.cpp src/flow32/graphics/Display.cpp src/InternalAllocation.cpp \
  -o "$test_dir/memory_display_test"
"$test_dir/memory_display_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Itests/host/stubs -Isrc tests/host/touch_test.cpp -o "$test_dir/touch_test"
"$test_dir/touch_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Isrc -I.pio/libdeps/bunty/ArduinoJson/src tests/host/dashboard_cache_test.cpp -o "$test_dir/dashboard_cache_test"
"$test_dir/dashboard_cache_test"
