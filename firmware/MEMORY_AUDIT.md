# Bunty memory and concurrency audit

Audited and refactored on 2026-09-07. Scope: `firmware`, ESP32-S3,
PlatformIO espressif32 7.0.1, Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7,
ArduinoJson 7.4.3, Helix 0.9.4. No hardware was flashed.
Includes the subsequent WebRTC VAD change; see [VAD_TUNING.md](VAD_TUNING.md).

## Measured build results

| Measurement | Before (`bunty`) | After (`bunty`) | Change |
| --- | ---: | ---: | ---: |
| PlatformIO static RAM | 70,604 B | 77,988 B | +7,384 B |
| PlatformIO flash | 1,758,129 B | 1,773,225 B | +15,096 B |

The additional `bunty-no-psram` build passes with **77,820 B static RAM** and
**1,769,545 B flash**. The same source supports both builds.

These are linker/build measurements, **not live free-heap measurements**.
Static RAM does not include task stacks, driver allocations, TLS sessions, or
the framebuffers allocated at startup. This change deliberately reserves more
internal memory for predictable control/audio operation; it is not a claim
that total internal use decreased.

No ESP32 serial device was connected. Before/after free internal heap, minimum
heap, largest block, PSRAM use, stack high-water marks, display frame rate and
audio underruns therefore remain **unmeasured on hardware**. Do not substitute
the allocation budgets below for those measurements.

## Major placement changes

Bytes below are payload budgets unless noted; allocator metadata and RTOS/driver
objects are additional. Large buffers now fail explicitly instead of falling
back into the control heap.

| Allocation / data | Before | After |
| --- | --- | --- |
| RGB565 240×320 framebuffer | 153,600 B, PSRAM preferred, internal fallback | 153,600 B, PSRAM only |
| Monochrome framebuffer | None | 9,600 B internal; maintained alongside color and used alone without color PSRAM |
| Display transfer line | Direct framebuffer pointer, potentially PSRAM | 480 B `MALLOC_CAP_INTERNAL \| MALLOC_CAP_DMA`, allocated once |
| Display dirty tracking | Explicit caller rectangles only | 300 B static internal tile flags; changed pixels mark 16×16 tiles |
| Display driver and render mutex | Driver `new`, dynamic mutex | Driver is an embedded object; recursive mutex has static internal storage |
| Capture backlog | 307,200 B, PSRAM with ordinary-malloc fallback | 307,201 B PSRAM only, **307,200 usable** |
| Compressed playback backlog | 196,608 B, PSRAM with ordinary-malloc fallback | 196,609 B PSRAM only, **196,608 usable** |
| VAD pre-roll | 5,120 B PSRAM preferred | 9,600 B explicitly internal (300 ms; enlarged in VAD follow-up) |
| WebRTC VAD engine | None (RMS-only gate) | 756 B requested internal, allocated once; no per-frame/reset allocations |
| Upload frame | 1,024 B ordinary malloc at task start | 640 B embedded in the internal voice object (20 ms VAD frames) |
| Microphone raw slots / mono conversion | Stack, 384 B / 96 B | Same sizes on the internal capture task stack |
| Speaker stereo staging | 512 B stack | Same, internal playback/greeting stack |
| Decoded PCM window | 4,608 B PSRAM preferred | 4,608 B explicitly internal |
| Helix decoder working state | Library `AllocatorExt`: PSRAM preferred | 23,816 B requested internal, across eight allocations; linker wraps Helix allocation/free |
| Compressed MP3 input | 8,192 B reply / 4,096 B greeting, PSRAM preferred with fallback | Reply strictly PSRAM; greeting PSRAM when present, bounded internal allocation on a no-PSRAM module |
| Microphone I2S DMA | Six 64-frame stereo PCM32 buffers: 3,072 B | Same internal DMA buffers and internal descriptors |
| Speaker I2S DMA | Six 256-frame stereo PCM16 buffers: 6,144 B, repeatedly installed/uninstalled | Same internal buffers, retained after first use; transmitter stopped and amplifier muted between turns |
| I2S driver control objects | IDF ordinary malloc/calloc, potentially external under pressure | Task-scoped internal-only malloc/calloc during installation; failure does not spill into PSRAM |
| Dashboard raw response / NVS read buffer | Repeated malloc or HTTP `getString`, response unbounded before validation | One 12,289 B PSRAM allocation, 12,288 B maximum response plus terminator |
| Dashboard parsed snapshot | Default ArduinoJson heap allocations each refresh | Two reusable 65,536 B PSRAM arenas; rejected candidate leaves the live snapshot intact |
| Voice control / AI text | Temporary default JSON documents and Strings | 16,385 B PSRAM text assembly buffer plus 32,768 B PSRAM JSON arena; parsing borrows strings from that text buffer |
| Voice URI, headers, turn ID | Dynamic Strings | Internal fixed 256/384-byte arrays and atomic numeric turn ID, matching the gateway protocol |
| Display text / numeric labels | Repeated String copies, concatenation, truncation | Bounded static/stack character buffers; no heap allocation during rendering |
| BLE commissioning assembly | Growing String and copied `getValue()` | 4,336 B static internal assembly buffer; borrowed BLE frame data; 8,192 B setup JSON arena, freed when BLE stops |
| Diagnostics | Arduino `Serial.printf` mallocs above 63 characters | 256-byte stack formatting buffer; long log records truncated, protocol data unaffected |
| Fonts, certificates, greeting MP3, animation constants | Flash constants/assets | Remain in flash; no runtime asset copies introduced |
| Pairing, Wi-Fi, cache, gateway session | NVS | Remain in NVS |
| Servo calibration | Compile-time defaults | Validated NVS calibration, with existing defaults for absent/invalid values |

The normal PSRAM payload reservation, after dashboard and voice activation,
is approximately **858,116 B**. This includes the color framebuffer, software
audio backlogs, reply input, voice-control workspace, and dashboard workspaces;
it excludes TLS/network implementation details and the temporary pairing arena.
The eight Helix structures total **23,816 B**, measured using the ESP32-S3
compiler on `tests/host/decoder_budget.c`. Runtime diagnostics report their
actual allocated size, including allocator rounding.

The capture/playback backlogs are **software queues, not I2S DMA buffers**.
The ISR and I2S peripherals never receive their PSRAM addresses. PCM passes
through internal capture/playback staging and IDF-owned internal DMA buffers.

Without PSRAM, color becomes monochrome; the dashboard uses a 4,097-byte
internal body buffer and two 16,384-byte internal arenas. Oversized responses
are rejected, retaining a valid cached/live model when one exists. Cloud voice
is unavailable: its existing 9.6-second capture backlog and burst playback
budget cannot fit safely in internal RAM. Pairing, taps, servo control,
dashboard and the greeting remain supported. No new device-side history or
image subsystem was invented: conversation history is already owned by the
gateway, and existing graphics are fonts/vector drawings in flash.

## Task and ownership changes

All stacks and RTOS control blocks remain internal. The pinned IDF allocates
`xTaskCreatePinnedToCore` stacks internally. New application queues/mutexes use
static storage in internal RAM. Linker inspection confirms the voice, servo,
tap and display objects in DRAM (`0x3fc...`), with fonts and MP3 in mapped flash
(`0x3c...`).

| Task | Stack budget | Core / priority | Ownership |
| --- | ---: | --- | --- |
| Arduino loop | 8,192 B | 1 / 1 | UI, gestures, runtime scheduling, accepting dashboard results, NVS |
| Servo worker, per enabled axis | 2,048 B each | 1 / 4 | Eased trajectory and LEDC duty updates; fixed one-command queue, latest target wins |
| Tap detector | 3,072 B | 1 / 2 | Sole microphone reader; tap queue and capture producer |
| Dashboard network worker | 6,144 B | 0 / 1 | HTTP/mDNS fetch and unpair notification; fixed request/result queues |
| Voice network manager | 4,096 B | 0 / 2 | Socket start/stop, timeouts, playback acknowledgement |
| Voice upload | 4,096 B | 0 / 4 | VAD, pre-roll, PCM upload; sole capture consumer |
| Voice playback | 4,096 B | 1 / 5 | Sole reply speaker/decoder owner; blocking DMA writes pace execution |
| Greeting, temporary | 4,096 B | 1 / 3 | Greeting decoder/speaker, runs before voice activation |
| WebSocket, library-owned | 6,144 B configured | Library affinity / 5 | Bounded control parsing and playback producer; observed core logged |

Previously, dashboard HTTP/mDNS and voice connection cleanup ran on the same
loop as servo servicing. A TCP timeout could hold an intermediate servo target
for seconds. Both servo axes now have the same independent worker design;
**GPIO 8 turn is enabled, GPIO 7 tilt remains disabled**, matching the existing
wiring configuration. Their output remains LEDC hardware PWM, 50 Hz, 14 bits,
default 1–2 ms pulses. The backlight retains its independent 12 kHz LEDC setup.
No wiring or servo configuration constants changed. Voice tunables changed
in the subsequent VAD follow-up described in `VAD_TUNING.md`.

Cross-task voice flags/counters use atomics. Teardown requests a generation
that upload and playback must each acknowledge at a safe point; only then is
the socket stopped and the playback ring reset. Speaker setup/stop no longer
races writes from another task. Shutdown joins workers before freeing buffers;
if joining fails, buffers are retained and activation cannot overlap live
workers. Capture shutdown joins its producer through a mutex instead of
assuming a 30 ms delay is sufficient. VAD counters/pre-roll are upload-owned.

The static FreeRTOS stream buffer needs an extra sentinel byte. Adding that
byte makes capture's usable capacity even, preventing a short write into a
full ring from splitting PCM16 samples. Capture overruns are counted and
logged; playback overflow aborts the incomplete turn instead of silently
discarding compressed bytes. Bounded I2S writes and failed rate changes also
abort the reply cleanly.

## Display and ISR audit

Drawing changes the offscreen image and marks affected tiles. Presentation
merges adjacent dirty tiles and transfers one internal line at a time. An
explicit partial rectangle clears a dirty tile only if the entire tile was
presented, so other pending pixels are not lost. Full-screen redraws and the
existing mouth, eyes, waveform, top-bar and pairing-dot rectangles still work.
The render mutex protects drawing, the shared text scratch buffer and SPI.

The existing Arduino/Adafruit SPI backend is synchronous and uses the SPI FIFO.
The staging buffer is DMA-capable, but this refactor **does not claim actual
asynchronous display DMA or scanout/VSync synchronization**. That would require
a transport/backend change and hardware validation. The existing hardware
I2S DMA implementation is retained.

There are no application-defined interrupt handlers in this firmware. LEDC
generates servo pulses autonomously. I2S interrupts and DMA descriptors belong
to IDF; BLE/WebSocket callbacks are task callbacks, not ISRs. No application
allocation, logging, JSON parsing or PSRAM access has been added to an ISR.
Flash/NVS operations still impose the SDK's cache/scheduling constraints;
internal placement alone is not a guarantee of uninterrupted task execution.

The I2S scope wraps only ordinary malloc/calloc on the installing task; all
other tasks keep the SDK allocator policy. Both relevant I2S allocation sites
were verified to call the wrappers in the linked disassembly. Helix allocation
sites are separately wrapped for the decoder's entire lifetime.

## Remaining library allocations and monitoring

HTTPClient, mDNS (`MDNS.txt`, error descriptions), TLS/WebSocket, BLE, Wi-Fi,
NVS and initial driver/task creation still allocate internally. The project
does not vendor/rewrite those libraries. Large bodies and parsed text are now
bounded and explicitly placed; URI/authentication/discovery strings are cached
at runtime startup. The application rendering, servo, PCM processing and
voice control parsing loops do not use repeated system malloc/new/String.
This is not a claim of a globally allocation-free networking stack.

The installed SDK has `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=1` and no reserved
internal malloc pool. TLS remains an internal-heap consumer; runtime headroom
must be measured. Large application data cannot silently fall back into it.

The heartbeat now reports internal, PSRAM and DMA-capable free bytes, lifetime
minimum free bytes, largest free block, allocated bytes, framebuffer placement,
dirty tiles and transferred pixels, JSON arena usage, decoder allocation size,
and observed task cores/stack minima. Stack high-water marks are **bytes** in
this IDF version. Task samples are taken at most every five seconds per task;
retired tasks such as the greeting retain their last recorded minimum.
`frag = 100 × (1 - largest_free_block / total_free_bytes)` is a fragmentation
indicator, not a proof that multiple heap regions are fragmented. The DMA and
internal capability measurements overlap and must not be added together.

## Verification and hardware follow-up

Completed:

- Baseline and both final firmware builds; no compiler warnings in final build output.
- Host tests of the actual Display, arena, WebSocket assembly and scoped
  allocation code, with UBSan/bounds sanitizers and allocation canaries.
- Repeated color/monochrome rendering with no allocation calls after begin;
  internal line placement, clipping, unchanged tiles, partial-tile retention,
  allocation failures and cleanup.
- 1,000 repeated JSON parses and 1,000 two-arena snapshot swaps; arena growth,
  bounded allocation failure and preservation of realloc contents.
- Split, out-of-order, oversized and invalid WebSocket controls, binary
  continuations, and independent allocation policy for unrelated tasks.
- ESP32-S3 decoder size, DRAM/flash symbol placement, I2S allocation wrappers,
  unchanged pins and `git diff --check`.

ASan on this Mac deadlocked inside its dyld/malloc initialization before main;
a process sample confirmed that runtime failure. UBSan/bounds checks and
canaries passed. The test runner supports ASan on a working host.

Hardware work still required: record baseline and refactored logs at boot,
after pairing, Wi-Fi/TLS connection, capture, sustained playback and after at
least 100 repeated turns. Compare free/largest/minimum internal heap and PSRAM,
check for a declining post-turn baseline, and check every task's stack margin.
Exercise Wi-Fi loss, stalled Gauge requests, rejected/oversized replies, sleep,
wake and unpair while observing servo timing and audio overruns. Use a logic
analyzer to verify PWM remains 50 Hz during TLS/display activity. Repeat on a
no-PSRAM module and check readable monochrome fallback and rejection of
oversized dashboard responses. Visual smoothness, audio quality, physical
calibration and sustained TLS headroom are not established by host tests.

Reference basis: the installed framework headers/source and
[ESP-IDF 4.4 heap capabilities](https://docs.espressif.com/projects/esp-idf/en/release-v4.4/esp32s3/api-reference/system/mem_alloc.html),
plus the pinned [I2S implementation](https://github.com/espressif/esp-idf/blob/v4.4.7/components/driver/i2s.c)
for DMA allocation, control allocation and sample-rate changes.
