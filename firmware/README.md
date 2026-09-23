# Bunty ESP32-S3 firmware

Bunty is the ESP32-S3 reference implementation of the vendor-neutral
[Gauge Accessory Protocol](https://github.com/manuu-r/homebrew-tap/blob/main/docs/accessory-protocol.md). It pairs using
standard Bluetooth LE Secure Connections, joins Wi-Fi, fetches the Gauge
dashboard, and retains the last valid snapshot for offline display. Flow32,
eyes, touch, sleep, and servos belong to Bunty rather than the protocol.
There is currently no speaker or microphone support.

## Layout

Every pin, protocol constant, colour, and behavioural tunable lives in
`src/Config.h`. Nothing else in the firmware hard-codes a GPIO or a
threshold, so a new enclosure or panel revision is a change to that one file.

| File | Owns |
| --- | --- |
| `Config.h` | Wiring, protocol constants, colours, timings, NVS keys |
| `main.cpp` | Boot, the runtime state machine, and the post-pairing greeting eyes |
| `Ui.*` | The panel, framebuffer, render mutex, backlight, static screens |
| `Pairing.*` | BLE Secure Connections commissioning |
| `Dashboard.*` | Bonjour discovery, the authenticated pull, the NVS snapshot, pages |
| `Touch.*` | CST816D touchscreen, presses and holds |
| `Animations.*` | Eye frames |
| `ServoMotion.*` | Non-blocking eased servo glide |
| `Memory.*`, `Log.h` | Bounded JSON arenas, heap/stack diagnostics, allocation-free logging |
| `InternalAllocation.*` | Internal-only allocation scope for IDF I2S control structures |
| `flow32/graphics/` | `Display` (framebuffer `Adafruit_GFX`) and its ST7789 transport |

## Pairing

1. An unpaired Bunty advertises its Gauge GATT service and shows **PAIR
   BUNTY**.
2. Click **Pair Accessory…** in the Mac app.
3. macOS and Bunty show the same six-digit numeric-comparison value.
4. Confirm on the Mac, then press **Yes** on the screen. **No** rejects pairing.
5. Gauge sends Wi-Fi and its per-device dashboard credential over the
   MITM-protected link.
6. Bunty joins Wi-Fi, shows its greeting eyes, and begins showing Gauge data.

The link is encrypted and authenticated with numeric comparison, and Bunty
bonds for that session. macOS will not finish the encrypted read without the
bond. Gauge forgets its copy of the link key before the next attempt, and
Bunty clears its own before advertising again, so the number is compared
every time.

Bunty's static random Bluetooth address is minted once and kept for the life of
the device, including across unpairing. Rotating it per attempt is what made
macOS file every attempt as a separate accessory.

Upgrading from a bonding build: remove every leftover `Bunty-*` entry under
System Settings › Bluetooth once after flashing. Those entries hold link keys
this firmware no longer has, and macOS will keep trying to use them.

## Touch controls

The CST816D screen uses I2C address 0x15, SDA GPIO48, SCL GPIO47 and IRQ GPIO46.
Missing touch hardware is
reported on UART and prevents local pairing confirmation.

- Pairing: compare the numbers, then press the on-screen Yes or No button.
- Dashboard and connection screen: touch the Settings button at bottom-left (a 56 x 50 pixel area), or long-press anywhere, to open settings.
- Settings: view connection status, disconnect, or unpair with explicit Yes/No confirmation.
- Dashboard: touch the microphone button at bottom-right to start a conversation.
- Conversation: touch anywhere to end it while Bunty listens, or to cut Bunty off mid-reply and talk again.
- Sleeping: touch the screen to wake. That touch does nothing else.

Unpairing preserves unrelated
settings and the stable Bluetooth address. UART logs report touch detection,
BLE connection, confirmation decisions and authentication results.

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

## Voice conversation

Tapping the mic starts conversation mode against the IoT gateway's `/v1/live`
route (Flux STT, Claude, Aura TTS). Bunty opens a WebSocket at once and streams
16 kHz linear16 audio from the microphones; the gateway's Smart Turn decides
when you have finished speaking. The reply comes back as 16 kHz linear16 PCM
and plays through the MAX98357. After playback Bunty acknowledges it, the
gateway closes that turn, and Bunty opens the next socket and listens again.
Conversation mode ends after 8 s without anyone talking.

Claude tags each sentence with an emotion (soft, happy, excited, angry, sad,
curious, sassy). The gateway strips the tag before speech and sends it as a
control message, so Bunty's face and head switch the moment that sentence's
audio reaches the speaker. `Face` draws the eyes, lids, blinks, glances and
mouth from signed distance fields straight into the Flow32 framebuffer, and
repaints only what changed. To judge faces without flashing:

```sh
tests/host/face_preview.sh /tmp/faces.png [mouth-open 0..1] [ms]
```

Set `cfg::ui::kFaceDemo` to show every face for five seconds on the device
instead of booting normally.

While listening, Bunty cocks its head. While thinking, it glances aside. While
speaking, the head performs a gesture pattern for the emotion Claude tagged the
current sentence with (soft, happy, excited, angry, sad, curious, sassy). The
switch lands when that sentence's audio reaches the speaker. Patterns live in
`cfg::dance::kStyles`.

Voice needs a gateway credential; see provisioning below. Serial logs every
phase change, the transcript (`[voice] heard:`) and the reply (`[voice] bunty:`).

## Build and flash

```sh
pio run --target upload
pio device monitor --baud 115200
```

Ordinary uploads make no Cloudflare calls and keep whatever gateway credential
is already in NVS. To give Bunty a gateway credential (first time, or to
rotate it), install the gateway tooling, authenticate Wrangler, and request a
provisioned flash:

```sh
cd ../iot-gateway && npm install && npx wrangler login
cd ../firmware
BUNTY_PROVISION_IOT=1 pio run --target upload
```

That stages a new token for the D1 device `bunty`, embeds it from the ignored
`.pio/generated/` directory, flashes, and activates the token only after the
upload succeeds. On first boot the firmware copies the token into NVS and
mints a new conversation-memory session. The previous token stays valid if
the build or upload fails.

For a manual factory reset, clear the NVS partition. `--target erase` wipes the
whole chip and requires a full reflash:

```sh
esptool.py --chip esp32s3 erase_region 0x9000 0x5000
```

## Display

Rendering goes through Flow32's `Display`, an `Adafruit_GFX` that paints into an
off-screen framebuffer. RGB565 uses 153,600 bytes of PSRAM; a 9,600-byte
monochrome framebuffer stays internal and provides a fallback without PSRAM.
Dirty 16×16 tiles are merged into adjacent spans, then copied or expanded into
a 480-byte internal DMA-capable line buffer for synchronous SPI transfer.
Unchanged tiles are skipped. The current Arduino SPI backend uses its FIFO;
the line buffer is DMA-capable but this is not an asynchronous DMA backend.
`src/Ui.*` owns the panel, the render
mutex, and every static screen; greeting, sleeping, and waking frames live in
`src/Animations.*`. Emotions use only chunky cyan robo-eye silhouettes rendered with
Flow32's anti-aliased rounded shapes; sleep adds a small drifting `ZZZ`.
Dashboard pages remain visible for 11 seconds before rotating.

See [MEMORY_AUDIT.md](MEMORY_AUDIT.md) for placement changes, build measurements,
task/core ownership, and the hardware verification checklist. It predates the
removal of voice, so its audio rows are historical.

Host checks: `sh tests/host/run.sh` (run a firmware build first to install the
pinned ArduinoJson dependency). The runner uses UBSan/bounds checks and memory
canaries; set `BUNTY_SANITIZERS=address,undefined` where ASan works.

Display body text uses Google Sans Flex Regular at 9 pt, while headings use
Bungee Regular at 16 pt. Both are embedded as printable-ASCII bitmap fonts;
their attribution and OFL-1.1 terms are in `FONT_LICENSES.txt`.

## Hardware map

The on-board ST7789T3 (240x320) sits on the shared LCD/SD SPI bus and has no
reset line, so the driver resets it in software. The microphones, amplifier and
servos are wired to the headers; the firmware drives only the servos and holds
the amplifier shut down.

| Function | GPIO |
| --- | ---: |
| TFT CS / DC / RST | 45 / 42 / — |
| TFT backlight | 1 |
| TFT MOSI / SCLK | 38 / 39 |
| Touch + IMU SDA / SCL / INT | 48 / 47 / 46 |
| Turn servo (SG90, yaw) | 9 |
| Tilt servo (SG90, pitch) | 14 |
| MAX98357 SD (held low) | 2 |

The MAX98357 BCLK / LRC / DIN (15 / 43 / 44) and MEMS mic SCK / WS / SD
(11 / 12 / 13) are wired but unused; their pins and I2S ports stay in
`src/Config.h`. The second microphone currently reads
undriven data and needs its L/R select and power checked.

The display, BLE pairing, Wi-Fi runtime, dashboard client, and servos are
active. OTA is not implemented.

Both servos home at 100 degrees. Before each dashboard page change one of them
sweeps 40 degrees to each side, driven by the non-blocking eased-glide
`ServoMotion` on a core-1 task, with commands passed through a fixed queue.
LEDC generates 50 Hz, 14-bit hardware PWM independently of these tasks.
Calibration is stored in NVS namespace `bunty-motion`: `turn-home`, `turn-min`,
`turn-max`, `turn-speed` (and equivalent `tilt-*` keys when tilt is enabled).
Absent keys receive the existing defaults: 100°, 1000–2000 µs, 30°/s.
`cfg::servo::kEnabled` parks it. Power it from a suitable 5 V supply rather than
the ESP32's 3.3 V rail and tie the servo ground to ESP32 ground.

### Wi-Fi and Gauge discovery

Bunty requires 2.4 GHz Wi-Fi. A `NO_AP_FOUND` join failure is reported to Gauge with network-name, band, and range guidance; authentication failures get a separate password/security message.

New Gauge commissioning documents include `server_ipv4`, stored as an optional same-subnet address hint. Dashboard fetches try that address first, then return to Bonjour after a connection failure. Renew an older pairing once to receive the hint. An unsuccessful first fetch shows a retry screen instead of staying on SYNCING; the corner controls remain available.

### Dashboard caching and touch feedback

Touch-coordinate labels and rings are removed from normal firmware. Bunty polls Gauge's in-memory dashboard every 30 seconds. The last valid quota data survives temporary empty/error responses while other sections can update, and a CACHED badge identifies retained quota data. Deliberately disabling all providers still clears them. Snapshots persist across restarts; timestamp-only updates do not rewrite flash, and failed persistence is retried on later fetches.

Page gestures alternate yaw and pitch on successive pages. Each gesture powers only one motor, so pitch is not starved by yaw and the 1500 mA rail budget is retained. UART logs integer-only servo targets; avoid floating-point printf in the small servo task.
