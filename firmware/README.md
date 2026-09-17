# Bunty ESP32-S3 firmware

Bunty is the ESP32-S3 reference implementation of the vendor-neutral
[Gauge Accessory Protocol](https://github.com/manuu-r/homebrew-tap/blob/main/docs/accessory-protocol.md). It pairs using
standard Bluetooth LE Secure Connections, joins Wi-Fi, fetches the Gauge
dashboard, and retains the last valid snapshot for offline display. Flow32,
eyes, audio, taps, sleep, and the optional IoT Gateway belong to Bunty rather
than the protocol.

## Layout

Every pin, protocol constant, colour, and behavioural tunable lives in
`src/Config.h`. Nothing else in the firmware hard-codes a GPIO or a
threshold, so a new enclosure or panel revision is a change to that one file.

| File | Owns |
| --- | --- |
| `Config.h` | Wiring, protocol constants, colours, timings, NVS keys |
| `main.cpp` | Boot, the runtime state machine, and Bunty's face during a turn |
| `Ui.*` | The panel, framebuffer, render mutex, backlight, static screens |
| `Pairing.*` | BLE Secure Connections commissioning |
| `Dashboard.*` | Bonjour discovery, the authenticated pull, the NVS snapshot, pages |
| `TapGestures.*` | Raw taps to intents: arm voice, wake, unpair |
| `TapInput.*` | The microphone: tap detection and the shared capture ring |
| `Audio.*` | `Speaker`, the shared `Mp3Stream`, and the boot greeting |
| `Voice.*` | Local VAD, the turn-scoped gateway socket, reply playback |
| `Animations.*` | Eye, mouth, and waveform frames |
| `ServoMotion.*` | Non-blocking eased servo glide |
| `Memory.*`, `Log.h` | Bounded JSON arenas, heap/stack diagnostics, allocation-free logging |
| `InternalAllocation.*` | Internal-only allocation scope for IDF I2S control structures |
| `ControlMessage.h` | Bounded WebSocket text assembly |
| `flow32/graphics/` | `Display` (framebuffer `Adafruit_GFX`) and its ST7789 transport |

## Pairing

1. An unpaired Bunty advertises its Gauge GATT service and shows **PAIR
   BUNTY**.
2. Click **Pair Accessory…** in the Mac app.
3. macOS and Bunty show the same six-digit numeric-comparison value.
4. Confirm on the Mac, then tap Bunty once. Two taps on Bunty reject pairing.
5. Gauge sends Wi-Fi and its per-device dashboard credential over the
   MITM-protected link.
6. Bunty joins Wi-Fi, says hello, and begins showing Gauge data.

The link is encrypted and authenticated but never bonded. Commissioning is one
act with one purpose, and everything afterwards runs over the LAN, so no link
key is stored on either side and macOS is left with nothing to remember.

Bunty's static random Bluetooth address is minted once and kept for the life of
the device, including across unpairing. Rotating it per attempt is what made
macOS file every attempt as a separate accessory.

Upgrading from a bonding build: remove every leftover `Bunty-*` entry under
System Settings › Bluetooth once after flashing. Those entries hold link keys
this firmware no longer has, and macOS will keep trying to use them.

## Tap controls

The MEMS microphone is sampled continuously on `I2S_NUM_1`, independently of
network requests and screen rendering. The adaptive detector logs each accepted
impulse and its current noise floor over serial.

- During numeric comparison: one tap confirms, two taps reject.
- While viewing the dashboard: one firm tap wakes voice for a short
  conversation window (Bunty shows its face and listens); each reply extends
  the window and it lapses after about 25 seconds of silence.
- While viewing the dashboard: three **firm, evenly spaced** taps request
  unpairing. Soft impulses, fast echoes, and irregular timing are ignored.
- On the unpair screen: pause for one second, then make one more firm tap to
  confirm. Waiting six seconds cancels.
- After two minutes without interaction, the stats give way to Bunty's dimmed
  sleeping emotion at roughly 3% backlight brightness. One firm tap restores
  full brightness, plays the wake emotion, and returns to the latest dashboard
  page.

Confirmed unpairing asks an available Gauge Mac to revoke the device token,
then clears its local Gauge server identity, bearer token, Wi-Fi credentials,
and cached dashboard. It preserves unrelated device settings such as the
firmware-provisioned voice gateway credential and the Bluetooth address, so a
re-paired Bunty is recognisably the same accessory. Tap sensitivity and debounce
constants live in `src/Config.h` and should be calibrated on the assembled
enclosure.

## Dashboard and offline behavior

On every paired boot, Bunty reconnects with the saved Wi-Fi credentials and
uses Bonjour to find the Gauge service whose server ID matches the pairing
bundle. It requests `/v1/dashboard` with its per-device bearer token, validates
the protocol/schema, and stores the complete response in NVS only when it
changes. The display rotates through:

- AI quota overview;
- each provider's individual limit windows;
- upcoming Calendar events;
- to-dos.

When Wi-Fi or the Mac is unavailable, the last validated snapshot remains on
screen as **CACHED**. Failed discovery/fetch attempts retry every 15 seconds;
successful refreshes honor Gauge's interval, clamped to 30–900 seconds.
Display sleep does not stop Wi-Fi or dashboard refreshes, so the cached data is
kept current while the stats are hidden. Bunty disables ESP32 modem sleep for
this always-on LAN connection, refreshes Bonjour after a failed socket, and
reconnects its Wi-Fi station after three consecutive connection failures.

## Build and flash

The gateway is not required for ordinary builds or uploads. Once the gateway is
live, install its tooling and authenticate Wrangler before the first
**provisioned** flash:

```sh
cd iot-gateway
npm install
npx wrangler login
npm run db:migrate:remote
```

Ensure `iot-gateway/wrangler.jsonc` contains the real remote D1
`database_id`. Provisioning is deliberately opt-in. An ordinary upload performs
no Cloudflare or network operations and leaves an existing NVS credential
untouched:

```sh
cd ../firmware
pio run -e bunty --target upload
```

When the IoT Gateway is live, request a provisioned flash explicitly:

```sh
BUNTY_PROVISION_IOT=1 pio run -e bunty --target upload
```

Only the provisioned form:

1. stages a new random token for the D1 device named `bunty`;
2. embeds it into the firmware from Bunty's ignored `.pio/` directory;
3. flashes the board;
4. activates the new token after a successful upload.

The previous token remains valid if compilation or upload fails. If the flash
succeeds but the final activation call fails, the staged token is still
accepted. PlatformIO leaves a non-secret marker under `.pio/`, retries that
activation before the next flash, and also prints a manual recovery command.

Normal non-upload builds are also offline:

```sh
pio run -e bunty
pio device monitor --baud 115200
```

### Board variants

| Env | Board | Notes |
| --- | --- | --- |
| `bunty` | Original carrier board | Default. |
| `bunty-no-psram` | Carrier board, no PSRAM module | Monochrome display, no cloud voice; pairing, dashboard, taps, greeting and servo persist. |
| `bunty-waveshare` | Waveshare ESP32-S3-Touch-LCD-2 (2.0", 240x320) | Same ESP32-S3R8 module and octal PSRAM, so voice, UI, pairing and dashboards are unchanged. The display wiring differs and the MAX98357 LRC moves to GPIO18 (GPIO5 is not broken out); every other servo, amplifier and microphone pin matches the carrier board. Serial runs over the board's native USB port, not a UART bridge. Provisioning flags (`BUNTY_PROVISION_IOT=1`) work the same. |

```sh
pio run -e bunty-waveshare --target upload
```

At boot, Bunty copies the compiled credential into its `bunty` NVS namespace
under `iot_token` and records `iot_device=bunty`. This credential is separate
from the local Gauge dashboard token stored under `token`; pairing and dashboard
authentication are unchanged. The plaintext IoT credential is never written to
the repository or printed during the flash, but it is present in the firmware
image and on the device. Use ESP32 flash encryption or a secure element if
physical extraction is in scope.

The cloud device ID is intentionally fixed to `bunty` for now. Before flashing
more than one physical Bunty, change the hook to derive a stable unique ID per
board; otherwise the most recent flash will rotate the credential used by the
previous board.

For a manual factory reset, clear the NVS partition. `--target erase` wipes the
whole chip and requires a full reflash:

```sh
esptool.py --chip esp32s3 erase_region 0x9000 0x5000
```

## Voice

Voice is dormant until a firm tap on the dashboard arms it, so a quiet room is
never streamed to the cloud. Once armed, the device keeps a short microphone
pre-roll, opens `/v1/live` when WebRTC VAD detects sustained speech, streams 16 kHz
mono linear PCM after `session.ready`, and lets the gateway's Flux/Smart Turn
decide the endpoint. The firmware no longer sends local `input.end` controls.
Each completed reply re-arms for another window;
after **four seconds without speech** the window closes and another tap is needed. The
gateway then finalizes Flux, asks Claude for a short reply, streams Aura's MP3
reply back, and closes the turn after Bunty reports that playback has drained.
As a transport failsafe, Bunty also closes a live WSS turn after 20 seconds with
no microphone PCM, gateway control/audio, or audible local playback; WebSocket
ping/pong traffic does not count as conversation activity.

The four-second limit applies while waiting for the user **and while uploading
input**, not during processing/playback after a confirmed gateway endpoint.
Noise rejected by VAD counts as non-speech, even if loud. While uploading,
four seconds without sustained local speech or without evolving gateway
transcripts sends `session.close`, stops the socket and disarms until a new tap.
PCM sends, pings, candidate turns, empty/repeated transcripts and isolated
positive VAD frames cannot keep the speech watchdog alive. A stalled recognizer
can also trip the transcript watchdog; this is not a semantic nonsense filter.
TLS/setup retains its separate connection timeout and buffered microphone PCM.
There is already no socket during idle local listening. Expiry does not erase
the gateway's stored conversation history.
WebRTC runs in mode 2 on 20 ms / 320-sample frames, with 160 ms onset confirmation,
300 ms pre-roll, and a small 100-RMS guard against very low-level noise.
The RMS meter alone no longer decides whether sound is speech. Configuration
and rationale: [VAD_TUNING.md](VAD_TUNING.md), `src/VoiceActivity.h`.

**Deploy the updated gateway before flashing this firmware.** The device opts
in with `X-Bunty-Turn-Detection: gateway` and requires
`session.ready.turn_detection=smart-turn-v2` before sending PCM. An older
gateway is rejected cleanly; old firmware without the header remains compatible
with the updated gateway. Neither deployment nor flashing has been performed.

The ignored build credential is copied to NVS at boot. Runtime activation reads
that stored `iot_token`, so an ordinary later build does not disable voice just
because its generated header was cleaned. Serial diagnostics use UART0 through
the board's CH343 USB bridge and include the current speech decision, RMS/guard,
and estimated non-speech noise level. The heartbeat is sampled, not an event trace.

## Audio

`src/audio/greeting.mp3` is embedded by `src/audio/greeting.S` and
decoded at boot. The Helix decoder is already linked for the voice gateway's
spoken replies, so `src/Audio.cpp` shares one `Mp3Stream` between the greeting
and the reply path and shipping the clip compressed costs no extra code --
about 215 KB less flash than a pre-decoded PCM blob.

Everything that reaches the MAX98357 on `I2S_NUM_0` goes through `Speaker`,
which installs the I2S driver on first use and retains its internal DMA buffers.
It stops the transmitter and holds the amplifier's shutdown pin low while idle.
The tap detector drains and suppresses
microphone events during playback so the speaker cannot trigger an accidental
unpair gesture.

## Display

Rendering goes through Flow32's `Display`, an `Adafruit_GFX` that paints into an
off-screen framebuffer. RGB565 uses 153,600 bytes of PSRAM; a 9,600-byte
monochrome framebuffer stays internal and provides a fallback without PSRAM.
Dirty 16×16 tiles are merged into adjacent spans, then copied or expanded into
a 480-byte internal DMA-capable line buffer for synchronous SPI transfer.
Unchanged tiles are skipped. The current Arduino SPI backend uses its FIFO;
the line buffer is DMA-capable but this is not an asynchronous DMA backend.
`src/Ui.*` owns the panel, the render
mutex, and every static screen; speaking, sleeping, and waking frames live in
`src/Animations.*`. Emotions use only chunky cyan robo-eye silhouettes rendered with
Flow32's anti-aliased rounded shapes; sleep adds a small drifting `ZZZ`.
Dashboard pages remain visible for 11 seconds before rotating.

See [MEMORY_AUDIT.md](MEMORY_AUDIT.md) for placement changes, build measurements,
task/core ownership, and the hardware verification checklist. Modules without
PSRAM fall back to a monochrome framebuffer and a 4 KB dashboard response limit
(the `bunty-no-psram` env; see Board variants). Cloud voice requires PSRAM for
its existing capture/playback buffering. Conversation history lives at the
gateway.

Host checks: `sh tests/host/run.sh` (run a firmware build first to install the
pinned ArduinoJson dependency). The runner uses UBSan/bounds checks and memory
canaries; set `BUNTY_SANITIZERS=address,undefined` where ASan works.

Display body text uses Google Sans Flex Regular at 9 pt, while headings use
Bungee Regular at 16 pt. Both are embedded as printable-ASCII bitmap fonts;
their attribution and OFL-1.1 terms are in `FONT_LICENSES.txt`.

## Hardware map

On the original carrier board the SmartElex 2-inch 240x320 IPS panel uses its
ST7789P3 controller in hardware-SPI mode. Connect the module's `EN` pin to
`TFT_BL`. The Waveshare variant is below.

| Function | GPIO |
| --- | ---: |
| TFT CS | 10 |
| TFT DC | 9 |
| TFT RST | 14 |
| TFT EN / backlight | 21 |
| TFT MOSI / SDA | 11 |
| TFT SCLK / SCL | 12 |
| Turn / pan servo signal | 8 |
| MAX98357 SD | 13 |
| MAX98357 BCLK | 4 |
| MAX98357 LRC | 5 |
| MAX98357 DIN | 6 |
| MEMS mic BCLK | 15 |
| MEMS mic WS | 16 |
| MEMS mic DATA | 17 |

### Waveshare ESP32-S3-Touch-LCD-2 (`-e bunty-waveshare`)

The display pins change, and the MAX98357 LRC moves from GPIO5 (not broken out
on this board) to GPIO18. Every other servo, amplifier and microphone pin above
is unchanged. The on-board ST7789T3 (also 240x320) sits on the board's shared
LCD/SD SPI bus and has no dedicated reset line, so `TFT RST` is `-1` and the
driver issues a software reset. The board has no microphone or amplifier of its
own: the same external MEMS mic and MAX98357 are wired to the pins above,
sharing the camera FPC header, so this build gives up the camera.

| Function | GPIO |
| --- | ---: |
| TFT CS | 45 |
| TFT DC | 42 |
| TFT RST | — (software reset) |
| TFT backlight | 1 |
| TFT MOSI / SDA | 38 |
| TFT SCLK / SCL | 39 |
| MAX98357 LRC | 18 |

The display, amplifier, microphone tap detector, voice, BLE pairing, Wi-Fi
runtime, dashboard client, and turn servo are active. There is no wake word:
voice is armed by a double tap. OTA is not implemented.

The servo on GPIO 8 homes at 100 degrees and sweeps 40 degrees to each side
before every dashboard page change, driven by the non-blocking eased-glide
`ServoMotion` on a core-1 task, with commands passed through a fixed queue.
LEDC generates 50 Hz, 14-bit hardware PWM independently of these tasks.
Calibration is stored in NVS namespace `bunty-motion`: `turn-home`, `turn-min`,
`turn-max`, `turn-speed` (and equivalent `tilt-*` keys when tilt is enabled).
Absent keys receive the existing defaults: 100°, 1000–2000 µs, 30°/s.
Tilt on GPIO 7 remains disabled until its existing wiring issue is resolved.
`cfg::servo::kEnabled` parks it. Power it from a suitable 5 V supply rather than
the ESP32's 3.3 V rail and tie the servo ground to ESP32 ground. Note that a
servo mounted near the microphone injects noise into both the tap detector and
the voice VAD.
