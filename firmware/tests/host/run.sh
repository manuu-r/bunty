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

vad_dir=.pio/libdeps/bunty/libfvad
# Build the real pinned C engine, not a mocked classifier. Instrument its only
# allocator so resets and frame processing are checked for heap churn.
for source in "$vad_dir"/src/*.c "$vad_dir"/src/vad/*.c "$vad_dir"/src/signal_processing/*.c; do
  object="$test_dir/$(basename "$source" .c).o"
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
    -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
    -Dmalloc=vad_test_malloc -Dfree=vad_test_free -c "$source" -o "$object"
done
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Isrc -I"$vad_dir/include" tests/host/voice_activity_test.cpp "$test_dir"/*.o \
  -o "$test_dir/voice_activity_test"
"$test_dir/voice_activity_test" "$vad_dir/tests/data/audio_tiny16.wav"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Itests/host/stubs -Isrc tests/host/touch_test.cpp -o "$test_dir/touch_test"
"$test_dir/touch_test"
