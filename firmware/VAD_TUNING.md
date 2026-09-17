# VAD selection and four-second idle timeout

Researched and implemented 2026-09-07, then updated for gateway-owned turn
detection and a four-second non-speech cancellation watchdog. No firmware was
flashed and no gateway was deployed.

## Findings and selection

There is no universally best ESP32-S3 VAD setting: microphone gain, distance,
enclosure acoustics, motor noise and playback echo determine the tradeoff.

Espressif's neural **VADNet** is available through ESP-SR's audio front end
(AFE). Its documented starting settings include 128 ms minimum speech,
1,000 ms minimum non-speech and a 128 ms onset cache. The cache matters because
classification and speech confirmation delay the first positive result.
[Espressif VADNet guide](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/vadnet/README.html)

AFE also supports noise suppression and echo cancellation with a playback
reference. This is a useful future direction for noisy/far-field or full-duplex
operation, not a measured quality win on this board. Bunty currently has one
microphone and deliberately ignores input while replying; adding AEC correctly
would require synchronized playback-reference audio.
[Espressif AFE guide](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html)

The inspected current ESP-SR component requires ESP-IDF >=5.0; this project
uses Arduino 2.0.17 / IDF 4.4.7. Moving to that AFE would entail an SDK/component
and model-packaging migration, beyond this VAD tuning change. Espressif's
published full AFE examples also consume substantial internal RAM and PSRAM;
those figures include other stages and are **not VAD-only costs**.
[ESP-SR dependencies](https://github.com/espressif/esp-sr/blob/master/idf_component.yml),
[ESP32-S3 AFE benchmarks](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/benchmark/README.html)

For this existing memory-constrained build, use **WebRTC VAD via libfvad**,
pinned to `532ab666c20d3cfda38bca63abbb0f152706c369`. It is a portable standalone
WebRTC speech classifier, replacing Bunty's RMS-only onset/release gate.
The API accepts 10/20/30 ms PCM frames. Higher WebRTC modes reject more noise
but also miss more speech. Mode 2 is our starting compromise; it is not a
claim that mode 2 is optimal for every microphone. VADNet's mode numbers do
not have interchangeable semantics.
[libfvad project](https://github.com/dpirch/libfvad),
[WebRTC/libfvad API](https://github.com/dpirch/libfvad/blob/532ab666c20d3cfda38bca63abbb0f152706c369/include/fvad.h)

## Installed configuration

All VAD tunables are in `src/VoiceActivity.h`; `Config.h` checks that capture
and VAD sample rates agree.

| Setting | Previous | Now | Reason |
| --- | --- | --- | --- |
| Detector | Adaptive RMS thresholds | WebRTC mode 2 + 100-RMS guard | Classify speech, not just loud sounds |
| Input | 16 kHz, PCM16 mono | Unchanged | Preserve microphone/gateway format |
| Frame | 32 ms / 1,024 B | 20 ms / 640 B | Valid WebRTC frame size; responsive decisions |
| Speech confirmation | 64 ms loudness | 160 ms consecutive positive frames | Reduce short-impact triggers, including short classifier hangover |
| Pre-roll | 160 ms / 5,120 B | 300 ms / 9,600 B | Cover confirmation and classification delay |
| Utterance endpoint | 800 ms low RMS | Gateway Flux + Smart Turn | Server owns semantic turn boundaries; no local `input.end` |
| Idle listening | 10 seconds | 4 seconds | Requested session timeout |
| Open-input watchdog | Transport activity could keep noise streaming | 4 s without sustained local speech OR transcript progress | Cancel unproductive input even when noise/PCM traffic continues |

The onset/watchdog durations are application policy, not extra libfvad API
parameters. Actual acoustic non-speech detection can be slightly later because WebRTC has
its own short hangover. The RMS guard uses the existing microphone's signal
scale; it is not a universal ESP32-S3 constant. Noise-floor estimation remains
diagnostic and no longer raises a moving threshold that can hide quiet speech.
All utterance PCM, including softer syllables and quiet frames, is uploaded
unchanged once the turn begins.

## Session semantics

- Tap: arm local listening for 4,000 ms; no socket opens without confirmed speech.
- No confirmed speech: disarm at the deadline (checked by voice-net's 10 ms loop);
  another tap is required. No idle cloud socket remains to close.
- Confirmed speech: open a turn, send pre-roll, then consume buffered/live PCM.
  Preserve the onset across `session.ready`, even if the whole word is in pre-roll.
- Negotiate `X-Bunty-Turn-Detection: gateway`. Require the gateway to advertise
  `smart-turn-v2` in `session.ready`; otherwise close without uploading PCM.
- While uploading, retain continuous PCM and pause context for Flux/Smart Turn.
  Do not drop individual quiet/unvoiced frames or make a local one-second endpoint.
  Stop uploading on `input.ended` (also accept `input.pause`/`assistant.thinking`).
  Candidate/incomplete `turn_detection` and speech-resumed events are advisory.
- Four seconds of PCM without **sustained** local speech, or four seconds of
  wall time without sustained local speech/evolving transcript text, cancels
  input via `session.close`, then tears down the socket and disarms. Isolated
  VAD-positive frames, loud non-speech, traffic, pings, empty text and duplicate
  transcript updates do not refresh these checks. No dummy AI reply is requested.
- The transcript timer starts at `session.ready`; a slow/stalled recognizer can
  cause cancellation even during real speech. This is a deliberate bounded-input
  safeguard, not proof that a sound is meaningless. VAD/transcription cannot
  reliably reject TV speech, hallucinations, or semantically nonsensical words.
- After the reply drains and turn cleanup completes, offer a fresh four-second
  follow-up window. AI latency and Bunty's own playback are not user silence.
- Existing connect and application-transport watchdogs remain separate. Expiry
  does not delete server conversation history or change credentials.

The deadline uses one atomic value, so a timeout check cannot overwrite a
concurrent new tap. Millisecond wraparound is handled (the reserved-zero
deadline case expires one millisecond later). Partial microphone frames survive
read timeouts instead of being discarded. The PCM non-speech watchdog counts
audio frames even when a TLS backlog drains quickly; the wall clocks also catch
microphone/recognizer stalls. TLS/setup itself retains a separate connection
timeout while microphone audio buffers. Signed timestamp deltas handle concurrent
updates slightly newer than the checking task's sampled clock.

## Gateway changes and rollout

The physical `/v1/live` route previously left Smart Turn disabled; it was only
enabled for browser voice. The new opt-in header enables it per authenticated
device connection without changing older firmware's behavior. A Flux
`EndOfTurn` without a preceding eager event now starts the verifier/fallback
for gateway-owned sessions. Resumed speech invalidates pending decisions.

Deploy the updated `iot-gateway` **before** flashing this firmware. New firmware
fails closed with a diagnostic if the endpoint capability is absent. Old
firmware remains supported by the new gateway. Neither rollout action has
been performed here; the changes and verification are local.

## Memory and verification

The classifier's single allocation requests **756 B internal RAM on ESP32-S3**;
linked disassembly confirms `fvad_new` calls the existing internal-scoped malloc
wrapper. It is freed only after the voice workers join. Reset and processing
do not allocate; mode and sample rate are restored after every reset.
Pre-roll stays internal, the upload frame stays embedded/internal, and all large
audio queues remain PSRAM-only. No new task, DMA buffer, model partition, or pin
change was introduced. C files now use GNU C11 (required by libfvad); Arduino's
C++ standard is unchanged.

| Build measurement | Before this VAD change | After |
| --- | ---: | ---: |
| `bunty` static RAM | 78,356 B | 77,988 B |
| `bunty` flash | 1,765,549 B | 1,773,225 B |
| `bunty-no-psram` static RAM | 78,188 B | 77,820 B |
| `bunty-no-psram` flash | 1,761,845 B | 1,769,545 B |

Internal runtime payload increases by 5,236 B (4,480 B extra pre-roll + 756 B
classifier), while static RAM decreases by 368 B. Net known internal increase:
4,868 B, excluding allocator rounding. PSRAM reservations are unchanged. The
gateway-coordination follow-up adds only 16 B static RAM over the first VAD build.
The no-PSRAM variant still intentionally disables cloud voice.

Verification performed:

- Both PlatformIO builds pass.
- `sh tests/host/run.sh`: existing memory/display tests plus production VAD,
  onset/gateway endpoint signals, non-speech and transcript watchdogs,
  four-second boundary, re-arm, wraparound, concurrent timestamps, invalid frame,
  allocation-failure, and allocation-free reset/processing tests pass under
  UBSan/bounds instrumentation.
- The pinned library's 16 kHz speech fixture is processed unmodified; every
  wrapper decision matches a fresh explicitly configured mode-2/16-kHz engine.
  Observed 257/270 positive frames on this host (760-byte host instance). This
  is a regression/smoke test, **not a board accuracy benchmark**; the older
  upstream golden file differs by one frame on this host.
- Gateway suite: 43/43 pass, plus syntax checks. Tests include actual route
  capability negotiation, legacy compatibility, non-eager endpoints, resumed
  speech cancelling a stale verifier, and session.close without an AI reply.
  Bounded 640-byte PCM frames remain supported. No deployment performed.
- No ESP32 serial port is available; heap/stack margins, CPU load and real
  acoustic false-positive/false-negative rates remain unmeasured.

## Hardware acceptance checks

1. Tap and remain quiet: armed becomes false after four seconds; no socket opens.
2. Speak a short word immediately after tapping: retain its beginning, submit
   after the pause, play the full reply, then close the quiet follow-up window.
3. Speak with natural pauses: only the gateway's confirmed endpoint stops input;
   a candidate/incomplete result must not stop the uploader.
4. Repeat with fan noise, servo movement, a desk impact, and quiet speech at the
   intended distance. Log speech decisions/RMS and task-stack/heap diagnostics.
5. If quiet speech is missed, compare mode 1 and/or lower the RMS guard using
   recordings; if noise triggers persist, evaluate mode 3 versus missed words.
   Mode changes alone cannot guarantee rejection of TV/music or speaker echo.
6. Test slow TLS, Wi-Fi loss, and repeated replies. The user-idle timeout must
   not interrupt accepted AI work or playback, and buffers must remain bounded.
7. After a valid onset, supply four seconds of fan/impact noise. Confirm
   `session.close`, socket teardown and disarming, even while PCM/pings continue.
   Test continuous false-positive VAD with no evolving transcript separately.
