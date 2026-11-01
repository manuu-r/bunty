# Bunty ESP32-S3 firmware

Bunty is a desk companion powered by Muse. The firmware builds with **native
ESP-IDF 6.0.1** and provides Muse pairing, local VAD for hands-free turns,
captions, touch, animated faces, and two servos. Muse supplies reply text;
the existing Cloudflare IoT gateway synthesizes it with Aura TTS.
Muse can also fetch/display JPEG, RGB565 and animated GIF images and show wrapped, paged text on the LCD.
See [MUSE.md](MUSE.md) for credentials, setup, and current verification limits.

## Build

Install ESP-IDF 6.0.1 and its ESP32-S3 tools. On macOS, install the prerequisites
from Espressif's [setup guide](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/get-started/linux-macos-setup.html), then:

```sh
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0.1
~/esp/esp-idf-v6.0.1/install.sh esp32s3
export IDF_PATH="$HOME/esp/esp-idf-v6.0.1"
cd firmware
./scripts/idf.sh build
```

The wrapper activates IDF and runs from the firmware directory. It also finds
`~/esp/esp-idf-v6.0.1`, `~/esp/esp-idf-v6`, or the temporary toolchain prepared
for this migration. Alternatively, activate IDF's `export.sh` yourself and run
`idf.py -B build-muse -D SDKCONFIG=build-muse/sdkconfig build`. The IDF version and external component versions are pinned.
The wrapper defaults to `build-muse/` with an independent sdkconfig. Artifacts
are `build-muse/bunty.bin`, `build-muse/bunty.elf`, and `build-muse/flash_args`.
The earlier `build/` image remains available for recovery.
PlatformIO/Arduino is no longer the build entry point.

This target uses the current Waveshare ESP32-S3-Touch-LCD-2 wiring in
`src/Config.h`, 8 MB flash, and 8 MB octal PSRAM. Other carrier boards need their
own pin map and memory settings; this build does not select a board automatically.

## Flash and verify

List ports with `ls /dev/cu.usb*` on macOS. Substitute your device's port:

```sh
./scripts/idf.sh -p /dev/cu.usbmodemXXXX flash monitor
```

Exit the monitor with Ctrl-]. If necessary, hold BOOT, tap RESET, release BOOT,
and retry. **Use a normal flash; do not erase flash for the first migration.**
`partitions.csv` preserves the previous 8 MB layout, including NVS at `0x9000`
with size `0x5000`. Calibration, gateway TTS credentials, power state, and Muse
setup survive a normal flash. Old unused NVS records are not erased. Initialization
errors stop boot with a diagnostic rather than erasing NVS or repeatedly aborting.

Verify on hardware in this order:

1. Boot: capture `[boot] previous reset=...`, heap/stack reports, and any panic.
2. Display/touch: idle face, Settings, pairing confirmation, and sleep/wake.
3. Muse setup: pair from the phone app using a real SDK token. Successful fresh
   provisioning reboots once so BLE does not compete with TLS/audio for internal RAM.
4. Motion: Muse head commands respect the saved calibration and load budget.
5. Voice: tap once, speak, pause, hear the complete reply, then speak again.
   Interrupt playback by tapping and verify old audio does not replay.
6. Reboot: Muse setup, gateway credentials, and calibration remain intact.
7. Display commands: request JPEG, RGB565 and animated GIF images, paged text, cancellation,
   a failing URL, and restoration of the face during/after a voice conversation.

The binary build and host checks cannot verify wiring, Muse phone interoperability,
actual room audio, amplifier timing, or live heap headroom. See [MUSE.md](MUSE.md)
for current verification limits. [MIGRATION.md](MIGRATION.md) describes the earlier
IDF migration and is historical.

## Gateway credentials

Ordinary builds/flashes make no Cloudflare calls and retain the credential
already saved in NVS. An existing ignored
`.pio/generated/IotGatewayCredentials.generated.h` is still accepted so the
migration can reuse the previously provisioned build credential. Never commit
that directory or expose its contents in logs.

For first provisioning or an intentional token rotation, install gateway tooling
and authenticate Wrangler, then explicitly run:

```sh
cd ../iot-gateway
npm install
npx wrangler login
cd ../firmware
python3 scripts/provision_iot_gateway.py --port /dev/cu.usbmodemXXXX --monitor
```

This stages a token, builds/flashes with IDF, and activates it only after a
successful flash. A failed build or flash leaves the old credential valid.
Activation recovery uses the existing ignored pending marker; a later run
retries activation before staging another token. Do not use this provisioning
command just to test the migration if Bunty already has a working token.

## Behaviour

An unpaired device advertises Muse's setup service. Confirm pairing on Bunty,
then configure Wi-Fi/account access in the Muse app. Muse owns Wi-Fi and NimBLE;
there is no Gauge provisioning, discovery, dashboard fetch/cache, or reconnect
worker. Bunty displays its animated face while idle.

Tap the microphone control to enter conversation mode. Local energy VAD opens
an utterance after speech begins, preserves 320 ms of audio, and sends after a
900 ms pause. Muse returns text; the gateway's `/v1/tts` route streams speech.
Capture pauses during playback, then listening resumes automatically. Eight
seconds of silence exits. See [MUSE.md](MUSE.md) for limits and tap behavior.

Touch Settings at bottom-left or long-press to open settings. Disconnect and
unpair have local confirmation. A wake touch performs only the wake action.

Power policy retains the current load budget, brownout recovery, backlight
limits, Wi-Fi transmit-power settings, and idle/active CPU frequencies. IDF
power management sets 80/240 MHz; automatic light sleep is disabled. The unused
simulated fuel gauge and its periodic flash writes have been removed.

## Layout

| Path | Responsibility |
| --- | --- |
| `src/Config.h` | Pins, timings, calibration defaults |
| `src/main.cpp` | Existing state machine, started by native `app_main` |
| `src/Muse.cpp` | Muse lifecycle, setup UI, commands, captions |
| `src/MuseDisplay.cpp`, `src/MuseScreenBuffer.h`, `src/MuseText.h` | Image staging, screen ownership, text wrapping/paging |
| `src/AudioIo.cpp` | IDF I2S channels, mic capture and speaker playback |
| `src/MuseVoice.cpp`, `src/MuseVad.h` | Muse turns, local VAD, gateway TTS playback |
| `src/Touch.cpp` | IDF I2C master and CST816D press tracking |
| `src/flow32/graphics/` | Flow32 framebuffer and native ST7789 transport |
| `lib/bunty_platform/` | Small IDF-backed runtime, NVS, Wi-Fi, PWM adapters |
| `.deps/gfx/` | Downloaded Adafruit GFX drawing code, adapted without Arduino |
| `.deps/arduinojson/` | Downloaded ArduinoJson 7.4.3, used without Arduino |
| `src/CMakeLists.txt`, `src/idf_component.yml` | Application build and external dependency manifest |

The LCD DMA transfer completes before its reusable eight-row DMA buffer is reused. Native
I2S gives mic and speaker separate controllers. Speaker DMA is cleared on reopen,
and the MAX98357 is muted while idle. Large frame/audio/JSON buffers stay in PSRAM;
ISR-visible driver state and DMA buffers stay internal.

See [RUNTIME.md](RUNTIME.md) for task ownership, buffer budgets, cancellation
rules, failure handling and the remaining hardware verification.

## Hardware map

| Function | GPIO |
| --- | --- |
| TFT CS / DC / software reset | 45 / 42 / no reset pin |
| TFT MOSI / SCLK / backlight | 38 / 39 / 1 |
| Touch SDA / SCL / interrupt | 48 / 47 / 46 |
| Yaw / pitch servo | 9 / 14 |
| MAX98357 shutdown / BCLK / LRC / DIN | 2 / 15 / 43 / 44 |
| Mic BCLK / WS / data | 11 / 12 / 13 |

Both microphone slots retain the current noise/energy selection logic. Power
servos from an appropriate 5 V supply with a common ground. OTA is not implemented.

## Host checks

```sh
sh tests/host/run.sh
python3 -B -m unittest discover -s tests/python -v
tests/host/face_preview.sh /tmp/bunty-faces.png
```

The first build or host check downloads checksum-pinned dependencies into ignored `.deps/`; subsequent runs use the cache. Host checks do not need a PlatformIO installation.
The checks cover memory placement, bounded JSON, capture-session handoff,
concurrent reply ownership, nonblocking fades, clipping, dirty regions,
monochrome fallback, touch debounce/holds/drag, VAD timing and pre-roll ordering,
actual font rendering, and legacy NVS compatibility. Python checks exercise gateway provisioning
order/failure recovery, adapted Muse chat parsing/initialization, and the actual Muse image downloader with a scripted
HTTP transport, without cloud calls or flashing.
`MEMORY_AUDIT.md` is a historical Arduino-era audit; its old build numbers and
audio budgets do not describe this IDF build. Use this build's size report and
actual device heap/stack logs for current measurements.

Font licensing is in `FONT_LICENSES.txt`. Library licences, versions and cache locations
are documented in `lib/README.md`.

## Screen shell

The face home and on-demand Agenda/Tasks/Notes/Info views share the existing display
with Muse images and the animated face. See [UI.md](UI.md) for tool parameters,
provider synchronization, resource limits and the
[screen preview](docs/bunty-shell-preview.png). Voice requests also ask Muse for a
bounded head gesture through `bunty.gesture`. These changes require flashing the
new build; cloud tool selection and physical servo timing remain to be verified.

The matte face renderer, sleep/wake sequence and voice attention poses are
described in [FACES.md](FACES.md).

The reply-to-speech pipeline, diagnostic stages and gadget listening UI and automatic VAD
are described in [VOICE.md](VOICE.md). Conversations stay active through screen
updates and silence. A subtle edge aura indicates listening, thinking or speaking
on every view; [preview](docs/bunty-aura-preview.png).

Animated GIF limits, memory ownership and checks are described in [GIF.md](GIF.md).
