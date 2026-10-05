# Existing-firmware migration to ESP-IDF 6.0.1

Implemented on 2026-10-05. This migration retains Bunty's Gauge dashboard and
Cloudflare voice behaviour. It does not add Muse or OTA, and no hardware was
flashed during implementation.

## Changes

The build moves from PlatformIO / Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7 to a
native ESP-IDF 6.0.1 CMake project. `app_main` initializes NVS, networking and
the event loop, then runs the existing application state machine. The gateway
Worker and its wire protocol are unchanged.

| Previous boundary | Native replacement |
| --- | --- |
| Arduino BLE classes | Bluedroid GATT attribute table and GAP callbacks |
| Blocking numeric-comparison callback | Main-task confirmation with the same touch Yes/No and timeout |
| Arduino Preferences | IDF NVS adapter retaining names, types and key semantics |
| Arduino Wi-Fi | IDF station driver, netif and Wi-Fi/IP event handlers |
| Arduino mDNS | Pinned `espressif/mdns` 1.9.1 |
| HTTPClient / WiFiClient | Numeric-host `esp_http_client`, bounded body callback and completion validation |
| Legacy I2S driver | Separate native standard-mode mic RX and speaker TX channels |
| Arduino Wire | IDF I2C master bus/device |
| Adafruit ST7789/SPI transport | IDF `esp_lcd` ST7789 on SPI2 with completion synchronization |
| Arduino LEDC / CPU controls | IDF LEDC timers/channels and power-management configuration |
| Arduino drawing/Print dependencies | Checksum-pinned GFX drawing code plus a minimal C++ Print interface |
| PlatformIO provisioning post-action | Explicit standalone stage → IDF flash → activate command |

`src/` contains Bunty's application code. Native platform adapters live in
`lib/bunty_platform/`; Adafruit GFX 1.12.6 and ArduinoJson 7.4.3 are fetched
from checksum-pinned release archives into ignored `.deps/`. The small library
build definitions live in `lib/`. No Arduino core is required. Registry-managed WebSocket 1.6.1 and mDNS 1.9.1 are pinned in
the manifest and dependency lockfile.

Bluetooth is stopped and its reserved memory is released before voice tasks and
microphone DMA start. An already-paired boot releases unused Bluetooth memory as
well. Re-pairing still uses the existing Settings reboot flow. The amplifier
remains muted while idle; every speaker reopen preloads silence across its DMA
buffers so an interrupted turn does not leave old samples to replay.

The current display path intentionally waits for each DMA completion. Flow32 has
one reusable line buffer, so returning early would let subsequent rendering
modify pixels still being transmitted. This migration does not claim a measured
frame-rate increase.

## Persistence and credentials

`partitions.csv` is identical to the old `default_8MB.csv` layout. NVS remains at
`0x9000`, size `0x5000`. No build or normal flash intentionally erases that range.
The NVS adapter retains u8 booleans/calibration, u16 ports, u32 power counters,
string credentials, float blobs, address blobs and cached dashboard blobs.
NVS initialization failure stops boot; it never invokes an automatic erase.

Existing gateway state stays in NVS. The old ignored generated credential
header under `.pio/generated/` remains a supported input. A normal build does
not stage, rotate or activate Cloudflare credentials.

## Validation completed

- ESP-IDF 6.0.1 ESP32-S3 firmware build and link passed.
- A separate clean build using `sdkconfig.defaults` passed.
- Final application binary: **1,617,152 bytes**; application slot: **3,342,336
  bytes**; **52% free**. Bootloader and application partition checks passed.
- IDF size report: DIRAM used **170,520 bytes**, with **171,240 bytes** remaining
  in that linker region. This is not a runtime free-heap measurement; driver
  allocations, task stacks, Bluetooth reclamation and PSRAM are not represented
  by that remaining number.
- Host checks passed for display memory/clipping/dirty regions/fallback,
  bounded JSON, touch tracking and dashboard-cache retention.
- Actual GFX and Bunty fonts passed text bounds, byte-count and drawing
  checks, with UBSan/bounds sanitizers.
- Native storage tests read fixtures using legacy NVS types and exercise empty
  Wi-Fi passwords, defaults, short buffers and commit failure.
- Four provisioning tests passed: activation after successful flash, no
  activation after failed flash, recovery-marker retention, and rejection of an
  invalid token without overwriting an existing header. All external calls were
  mocked; these tests did not contact Cloudflare or flash a device.
- Shell syntax and repository whitespace checks passed.

## First hardware verification

Use the commands and ordered checklist in [README.md](README.md#flash-and-verify).
Begin with a normal flash and the saved pairing. Check colours/orientation,
touch, Wi-Fi reconnect/discovery, cached dashboard, servo calibration, voice,
interruption, sleep/wake and persistence. Test new BLE commissioning afterward.

Outstanding evidence is hardware-specific: live heap/stack headroom, macOS BLE
interoperability, microphone slot order, speaker DMA timing, display performance,
and end-to-end voice reliability. No assumptions about those have been substituted
for a device test. `MEMORY_AUDIT.md` remains a historical Arduino-era document.
