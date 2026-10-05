# Bunty ESP32-S3 firmware

Bunty is a desk companion and reference client for the
[Gauge Accessory Protocol](https://github.com/manuu-r/homebrew-tap/blob/main/docs/accessory-protocol.md).
The firmware now builds with **native ESP-IDF 6.0.1**. It retains Gauge pairing,
Wi-Fi discovery, cached dashboards, touch, animated faces, two servos, and the
existing Cloudflare voice pipeline (Flux STT → Claude → Aura TTS).
Muse is not integrated in this migration.

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
for this migration. Alternatively, activate IDF's `export.sh` yourself and use
`idf.py` directly. The IDF version and external component versions are pinned.
Artifacts are `build/bunty.bin`, `build/bunty.elf`, and `build/flash_args`.
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
with size `0x5000`. The native storage adapter uses the same namespaces, keys,
and NVS value types for Gauge pairing, Wi-Fi, cached data, servo calibration,
IoT credentials, power state, and the stable BLE address. Initialization errors
stop boot rather than silently erasing NVS.

Verify on hardware in this order:

1. Boot: the console reports IDF 6.0.1, PSRAM, display, mic, and servo setup.
2. Display/touch: correct colours and orientation; dashboard and Settings work.
3. Existing pairing: Wi-Fi reconnects and Gauge data refreshes. Disable the Mac
   temporarily and check that the cached dashboard remains visible.
4. Motion: yaw and pitch alternate between pages, with the existing calibration.
5. Voice: tap the mic, speak, hear the complete reply, and check face/gesture
   timing. Interrupt a reply, talk again, and confirm no old audio replays.
6. Sleep/wake: touch wakes the display and dashboard polling continues.
7. Reboot: credentials, calibration, and cache remain intact.
8. When ready to test commissioning, unpair through Settings, then pair in
   Gauge using the matching six-digit code. Check both Yes and No decisions.

The binary build and host checks cannot verify panel wiring, BLE interoperability
with macOS, microphone slot order, amplifier timing, or live heap headroom.
See [MIGRATION.md](MIGRATION.md) for the migration details and validation record.

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

An unpaired device advertises the Gauge GATT service. Gauge reads Bunty's identity
and writes its sequenced commissioning document over a Secure Connections link
with MITM protection. Confirm the numeric comparison on both the Mac and Bunty.
Each new commissioning session clears previous bonds; the static random address
remains stable. Bluetooth is shut down after successful Wi-Fi commissioning. Its static memory
is then reclaimed before voice tasks and microphone DMA start.

Dashboard discovery tries the saved same-subnet IPv4 hint, then mDNS service
browsing and `gauge-<first 12 characters of server_id>.local`. Fetches are
bounded to 12 KB, parsed in reusable arenas, and authenticated using the saved
per-device bearer token. Last-good quotas survive temporary provider failures.
Timestamp-only changes do not rewrite the cache. Dashboard polling continues
while the display sleeps.

Touch the microphone button to enter conversation mode. Audio streams as 16 kHz
mono PCM over the authenticated `/v1/live` WebSocket. Flux/Smart Turn owns turn
completion. Claude's sentence emotion tags drive Bunty's faces and head gestures
as each sentence reaches the speaker. The existing playback acknowledgement,
per-turn socket lifecycle, interruption, and silence timeout are retained.

Touch Settings at bottom-left or long-press to open settings. Disconnect and
unpair have local confirmation. A wake touch performs only the wake action.

Power policy retains the current load budget, brownout recovery, backlight
limits, Wi-Fi transmit-power settings, and idle/active CPU frequencies. IDF
power management sets 80/240 MHz; automatic light sleep is disabled. Fuel values
are modelled consumption, not a physical battery measurement.

## Layout

| Path | Responsibility |
| --- | --- |
| `src/Config.h` | Pins, protocol constants, timings, calibration defaults |
| `src/main.cpp` | Existing state machine, started by native `app_main` |
| `src/Pairing.cpp` | Native Bluedroid GATT and Secure Connections |
| `src/Dashboard.cpp` | Native mDNS/HTTP, snapshot validation and cache |
| `src/AudioIo.cpp` | IDF I2S channels, mic capture and speaker playback |
| `src/Voice.cpp` | Existing gateway conversation protocol |
| `src/Touch.cpp` | IDF I2C master and CST816D press tracking |
| `src/flow32/graphics/` | Flow32 framebuffer and native ST7789 transport |
| `lib/bunty_platform/` | Small IDF-backed runtime, NVS, Wi-Fi, PWM adapters |
| `.deps/gfx/` | Downloaded Adafruit GFX drawing code, adapted without Arduino |
| `.deps/arduinojson/` | Downloaded ArduinoJson 7.4.3, used without Arduino |
| `src/CMakeLists.txt`, `src/idf_component.yml` | Application build and external dependency manifest |

The LCD DMA transfer completes before its reusable line buffer is reused. Native
I2S gives mic and speaker separate controllers. Speaker DMA is cleared on reopen,
and the MAX98357 is muted while idle. Large frame/audio/JSON buffers stay in PSRAM;
ISR-visible driver state and DMA buffers stay internal.

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
The checks cover memory placement, bounded JSON, clipping, dirty regions,
monochrome fallback, touch debounce/holds/drag, last-good dashboard retention,
actual font rendering, and legacy NVS compatibility. Python checks exercise gateway provisioning
order and failure recovery without cloud calls or flashing.
`MEMORY_AUDIT.md` is a historical Arduino-era audit; its old build numbers and
audio budgets do not describe this IDF build. Use this build's size report and
actual device heap/stack logs for current measurements.

Font licensing is in `FONT_LICENSES.txt`. Library licences, versions and cache locations
are documented in `lib/README.md`.
