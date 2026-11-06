# Runtime ownership and resource budgets

Bunty uses ESP-IDF 6.0.1 on ESP32-S3 with 8 MB PSRAM. Application bulk storage
is allocated at startup in PSRAM; DMA buffers, task stacks, synchronization and
interrupt-visible driver state stay in internal RAM. A failed bulk allocation
must not fall back into the internal heap used by Wi-Fi and DMA.

## Task ownership

| Task | Stack bytes | Priority / affinity | Owns |
| --- | ---: | --- | --- |
| Main | 8,192 | 3 / core 0 | UI state, framebuffer transactions, power policy, motion arbitration |
| Muse link | 12,288 | 4 / scheduler | SDK lifecycle and connectivity; SDK also owns its network workers |
| Muse voice | 8,192 | 4 / core 0 | SDK turn APIs, VAD, microphone ring reader, conversation phase |
| Muse TTS | 12,288 | 4 / core 0 | Gateway HTTP, frame decoding, response-slot cleanup |
| Muse audio | 4,096 | 5 / core 1 | Speaker I2S and its power claim, playback clock, cues, loudness |
| Microphone | 4,096 | 6 / core 1 | I2S RX, signal processing, capture ring producer |
| Touch | 2,560 | 2 / core 1 | I2C and press tracking; bounded event queue |
| Servo × 2 | 2,048 each | 4 / core 1 | One LEDC channel and trajectory per worker, 20 ms cadence |

Stack sizes include callees. Compiler `.su` reports describe individual frames,
not total call-chain use. The build emits these reports and warns for application
frames over 2,048 bytes. Use device `stack_min_free` logs to assess actual headroom;
TLS, SDK workers and interrupts are not covered by application frame sizes alone.

The UI uses a recursive mutex to keep checks of remote screen ownership and the
following drawing atomic. Clock synchronization runs outside that transaction.
Fades and timed notices advance from the loop; they do not sleep while holding
the UI mutex. Text/image SDK callbacks also hold this mutex while rendering.
HTTP/TLS ownership uses an independent atomic reservation, so TTS does not need
the UI mutex to acquire or release network capacity.

## Application bulk storage

| Buffer | Placement | Bytes / bound |
| --- | --- | ---: |
| Color framebuffer | PSRAM | 153,600 |
| Image staging | PSRAM | 153,600 |
| Microphone ring | PSRAM | 256,001 including stream-buffer sentinel |
| VAD pre-roll | PSRAM | 10,240 |
| Reply mailbox | PSRAM | Four 1,024-byte text slots plus generation/index metadata |
| TTS scratch | PSRAM | 1,281 network bytes, 640 PCM samples, 6,150-byte JSON body |
| Speech ring | PSRAM | 131,072 (4 s); its player and atomics stay internal |
| Reaction pictures | PSRAM | Four 2,048-byte slots; playback then uses the GIF path |
| LCD staging | Internal DMA | 3,840 |
| Monochrome fallback | Internal | 9,600 only if color allocation fails |

HTTP/TLS and SDK networking have additional dynamic allocations. This table is
not a whole-device heap total. Microphone and speaker DMA allocations belong to
their IDF drivers. Driver setup scopes force their ordinary malloc/calloc calls
into internal RAM without changing allocation policy for unrelated tasks.

The color display previously retained the monochrome buffer and used a 480-byte
DMA line. It now uses a single eight-row staging buffer without the redundant
monochrome copy: 6,240 bytes less internal RAM, and 40 instead of 320 submissions
for a complete screen. Dirty tiles, clipping, and failed-submit retries remain
supported. DMA completes before the staging buffer is reused. A hardware DMA
completion that never arrives still blocks the transport; we do not release a
buffer that may remain in use by hardware.

## Streaming and cancellation

- Only the voice task invokes Muse turn APIs. Reply publication is bounded and
  does not allocate. Queue overflow leaves the reply available in Muse/captions.
- The TTS worker sleeps on a notification when idle. It retains its mailbox slot
  through HTTP cleanup and speaker shutdown. Cancellation invalidates the
  generation; the voice task waits for old slots to drain before resuming capture.
- Cancellation cannot interrupt an IDF blocking call immediately. HTTP operations
  use 15-second connection/header timeouts and 3-second audio-read timeouts;
  playback retains a 60-second deadline and 1,920,000-byte limit.
- TTS JSON escaping uses fixed PSRAM scratch, including worst-case control-byte
  expansion. Reply truncation preserves UTF-8 boundaries. TLS still allocates via
  the IDF allocator.
- Capture sessions tag each I2S read. Frames from a previous session are dropped;
  stopping capture waits for a nonblocking ring write before discarding the ring.
  I2S is drained while idle, but filtering/RMS work is skipped.
- DSP and playback energy calculations use integer accumulation and single-
  precision square roots. Signed negative microphone samples are multiplied,
  avoiding undefined left shifts.
- Images stage a complete result before committing. Image and TTS HTTP work are
  mutually exclusive. Drawing or dismissing a screen cannot revoke a live HTTP
  reservation.

## Scheduling, power and failures

CPU/radio reconfiguration belongs to the main task. Conversation activity keeps
CPU/radio awake through cancellation cleanup. Worker-visible power mode is atomic;
load admission uses a critical section. Repeated denied load requests log once
until admission succeeds. Brownout recovery and current limits remain enabled.
The unused simulated battery gauge and its once-per-minute NVS writes were
removed; existing stored values and credentials are not erased.

Initialization failures release partially created I2C/I2S resources. Muse queue
and mutex allocation is checked before the voice task starts. SPI submission
errors return a failure and retain dirty tiles instead of aborting the device.
Permanent peripheral faults still require hardware diagnosis.

`[runtime]` logs report peak loop work time and iterations over 50 ms, excluding
the deliberate loop sleep. `[memory]` reports heap, largest blocks, stack headroom
and display traffic. `[muse-tts]` reports connection/playback progress and stack
headroom without reply text or credentials.

## Verification status

Host tests cover display placement, 40-transfer full screens, dirty-region retry,
unchanged rows, clipping, allocation failures, fallback, bounded JSON, fades across
clock wrap, capture cancellation races, microphone arithmetic, and concurrent
reply handoff. Python tests run the adapted SDK chat parser/recovery harness,
including queue/mutex initialization failures, image transport and provisioning.
The face preview renders all eight emotions through the actual framebuffer.

The firmware was flashed on 2026-10-06 through `/dev/cu.usbmodem101`, with the
written data hash verified. A bounded serial capture showed Wi-Fi and Muse
connected, BLE off, internal free heap 132,375 bytes (minimum 106,523),
and 7,608,888 bytes free PSRAM. Main stack minimum free was 5,804 bytes and
voice 6,452 bytes. No repeated reset appeared during this idle observation.
The initial loop report included a 447 ms peak and 63 iterations over 50 ms;
steady-state frame timing and reply playback remain unverified.
Hardware acceptance still requires repeated Muse
replies and interruptions, image/text display during conversation, reconnect,
and sustained runtime while watching reset reasons, heap minima, stack minima
and loop stalls. Host tests cannot establish frame rate, audio underruns, supply
stability or the cause of the previously reported resets.

## Screen snapshots and speech gesture ownership

The shell lazily adds two fixed PSRAM models on the first content/context update,
23,072 bytes each (46,144 total), without
a second framebuffer. Model updates validate in staging under the existing UI
mutex; rendering remains on the main task. Named speech gestures use one atomic
32-bit mailbox and the existing Dance owner/power claim. Conversation offsets
are capped at ±18° yaw/±15° pitch, rates at 60°/s; gesture trajectories themselves
use at most 12°. Manual Muse head requests defer while Dance owns or homes the
head. These bounds complement, rather than measure, the modeled rail budget.

2026-10-06 shell flash verification: the complete `link.register` is 8,892 bytes,
which exceeded the SDK's upstream 8 KB serialization limit. The Bunty adapter
now uses a single bounded 32 KB PSRAM print buffer, freed after the existing
chunked sender finishes. Generic SDK boards retain their original limit. The
corrected build flashed with hash verification and Muse acknowledged registration.
Boot observation showed Wi-Fi/Muse connected without repeated resets during
the 45-second capture. Internal free memory was 132,095 bytes (minimum 106,479),
largest internal block 63,488; PSRAM free 7,561,776. Main/voice stack headroom
was 6,620/6,460 bytes. This idle observation does not verify TTS or gesture timing.

The updated reply-to-speech pipeline and modern listening screen are documented
in [VOICE.md](VOICE.md). HTTP headers are captured into bounded storage directly
from events, and failed playback now stops the conversation with a specific
error. Silent/invalid streams cannot be reported as completed speech. The
reported no-speech symptom still requires device verification after flashing.

## Bounded voice upload

The utterance buffer adds 1,048,576 bytes of checked PSRAM allocation at voice
startup. Capture and network upload now run sequentially: the microphone task
continues its normal I2S draining, but capture pauses during upload. The existing
256,000-byte mic ring remains for scheduling variation. SDK PCM/base64 staging
and queued upload bodies use PSRAM; the six-slot / 16 KiB queue is unchanged.
See [VOICE.md](VOICE.md) for cancellation and deadline bounds. The host test
executes the actual buffered uploader against slow, cancelled, disconnected and
failed SDK operations and checks sample preservation and capacity limits.
