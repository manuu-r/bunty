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

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Isrc tests/host/muse_vad_test.cpp -o "$test_dir/muse_vad_test"
"$test_dir/muse_vad_test"

# Exercise the actual cached graphics/Print adaptation, not the display stub.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -I.deps/gfx/include -Itests/host/stubs -Ilib/bunty_platform -Isrc \
  tests/host/native_gfx_test.cpp .deps/gfx/Adafruit_GFX.cpp src/Fonts.cpp \
  -o "$test_dir/native_gfx_test"
"$test_dir/native_gfx_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -I.deps/gfx/include -Itests/host/stubs -Ilib/bunty_platform -Isrc \
  tests/host/muse_display_test.cpp .deps/gfx/Adafruit_GFX.cpp src/Fonts.cpp \
  -o "$test_dir/muse_display_test"
"$test_dir/muse_display_test"

# Read fixtures using the same NVS representations as Arduino Preferences.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Itests/host/stubs -Ilib/bunty_platform \
  tests/host/native_storage_test.cpp lib/bunty_platform/Preferences.cpp \
  -o "$test_dir/native_storage_test"
"$test_dir/native_storage_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Isrc tests/host/muse_reply_mailbox_test.cpp -o "$test_dir/muse_reply_mailbox_test"
"$test_dir/muse_reply_mailbox_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Isrc -I.deps/arduinojson/src tests/host/runtime_resources_test.cpp -o "$test_dir/runtime_resources_test"
"$test_dir/runtime_resources_test"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Imanaged_components/espressif__cjson/cJSON -c managed_components/espressif__cjson/cJSON/cJSON.c -o "$test_dir/cjson.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -I.deps/gfx/include -Itests/host/stubs -Ilib/bunty_platform -Isrc \
  -Imanaged_components/espressif__cjson/cJSON \
  tests/host/shell_test.cpp src/ShellData.cpp src/ShellView.cpp src/Fonts.cpp \
  .deps/gfx/Adafruit_GFX.cpp "$test_dir/cjson.o" -o "$test_dir/shell_test"
"$test_dir/shell_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -include tests/host/shell_clock_stub.h \
  -I.deps/gfx/include -Itests/host/stubs -Ilib/bunty_platform -Isrc -I.deps/arduinojson/src \
  -Imanaged_components/espressif__cjson/cJSON \
  tests/host/shell_commands_test.cpp src/Shell.cpp src/ShellData.cpp src/ShellView.cpp src/Fonts.cpp \
  src/flow32/graphics/Display.cpp .deps/gfx/Adafruit_GFX.cpp "$test_dir/cjson.o" -o "$test_dir/shell_commands_test"
"$test_dir/shell_commands_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Isrc tests/host/speech_gesture_test.cpp -o "$test_dir/speech_gesture_test"
"$test_dir/speech_gesture_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Itests/host/stubs -Isrc tests/host/face_render_test.cpp src/Face.cpp src/flow32/graphics/Display.cpp \
  -o "$test_dir/face_render_test"
"$test_dir/face_render_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Isrc tests/host/tts_stream_test.cpp -o "$test_dir/tts_stream_test"
"$test_dir/tts_stream_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Isrc tests/host/speech_player_test.cpp -o "$test_dir/speech_player_test"
"$test_dir/speech_player_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -I.deps/gfx/include -Itests/host/stubs -Ilib/bunty_platform -Isrc \
  tests/host/voice_screen_test.cpp src/VoiceScreen.cpp src/Fonts.cpp .deps/gfx/Adafruit_GFX.cpp \
  -o "$test_dir/voice_screen_test"
"$test_dir/voice_screen_test"

# The decoder's desktop path intentionally uses unaligned word loads. Keep all
# other sanitizers; the Bunty canvas/validator retains full alignment checking.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
  -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize=alignment -fno-sanitize-recover=all \
  -I.deps/animatedgif/src -c .deps/animatedgif/src/AnimatedGIF.cpp -o "$test_dir/gif.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -isystem .deps/animatedgif/src -Isrc tests/host/gif_test.cpp "$test_dir/gif.o" -o "$test_dir/gif_test"
"$test_dir/gif_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Isrc tests/host/buffered_upload_test.cpp -o "$test_dir/buffered_upload_test"
"$test_dir/buffered_upload_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Isrc tests/host/gif_transfer_test.cpp -o "$test_dir/gif_transfer_test"
"$test_dir/gif_transfer_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize="${BUNTY_SANITIZERS:-undefined,bounds}" -fno-sanitize-recover=all \
  -Isrc tests/host/visual_gesture_test.cpp -o "$test_dir/visual_gesture_test"
"$test_dir/visual_gesture_test"
