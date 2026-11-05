# Bunty review: faster, smoother, and able to do tasks

Reviewed 2026-11-05 against `main` (firmware, `iot-gateway`, tests, docs).
Nothing here was run on a device: this review environment has no ESP-IDF
toolchain and no hardware. Gateway tests (66) and the host VAD test pass.
Every item below marked **unverified** needs a flash and a spoken turn.

## How a turn flows today, and where the time goes

```text
mic -> VAD (900 ms pause) -> [whole note buffered] -> upload to Muse (6 KiB chunks)
    -> Muse STT + agent (+ tools) -> completed text reply
    -> NEW TLS handshake to gateway -> /v1/speak -> Aura per sentence -> PCM ring -> I2S
```

| Stage | Where | Cost | Fixable here? |
| --- | --- | --- | --- |
| End-of-speech wait | `MuseVad.h:44` (`quiet_ >= 14400`) | 900 ms every turn | yes, tunable |
| Upload starts only after VAD ends | `MuseVoice.cpp:112-141`, `BufferedUpload.h` | grows with note length (5 s note = about 27 chunks of 6 KiB, sent one after another) | yes, biggest win |
| Muse STT + agent | Muse cloud | not ours | no |
| TLS handshake per reply | `MuseVoice.cpp:300-311` creates a fresh `esp_http_client` per reply | 1 to 3 s on an S3 | yes |
| Gateway: auth (D1) + rate limit + first Aura sentence | `iot-gateway/src/index.js` `handleDeviceSpeech` | a few hundred ms | partly |
| 300 ms prebuffer | `SpeechPlayer.h:10` | 300 ms | tunable, low value |

## A. Faster (ranked by payoff)

1. **Hide the TLS handshake.** Every reply opens a new HTTPS connection, so the
   handshake plus certificate-bundle verification sits between "reply received"
   and the first sound, and again between two queued messages. Options, cheapest
   first:
   - Keep one `esp_http_client` handle for the life of the voice task (re-use it
     with `esp_http_client_set_url/set_method`, `open()` per reply) so keep-alive
     and TLS session tickets can work. Add `config.save_client_session = true` and
     `CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS=y`; a resumed session skips the
     certificate chain and key exchange.
   - Warm the connection while Muse is thinking (after `finish()`), with a cheap
     unauthenticated `GET /v1/ping` (204, a new gateway route) on the same handle. Muse takes seconds,
     the handshake takes one or two.
   - **unverified**; touch `fetchSpeech` only, the `streamTts` template and its
     host tests do not change.
2. **Upload while the user is still talking.** The design buffers the whole note
   and uploads after VAD ends (the right fix for the old queue-stall bug, but it
   moved all upload time after the pause). Keep the buffer, but run the upload
   from a second task that drains the buffer behind the capture cursor: call
   `muse_hatch_turn_begin()` on `Started`, send everything except the trailing
   quiet region while speech continues, and on `Ended` send the remainder and
   `turn_end()`. Capture never waits on the transport (the buffer is the
   decoupling), a false start is a `turn_cancel()`. A 5 s question then costs
   about 0.9 s after the pause instead of 2 to 3 s.
   **unverified**; needs the SDK's `muse_hatch_turn_audio` blocking behaviour
   checked in `.deps/muse` (not fetched here).
3. **Shorten the pause, adaptively.** 900 ms is conservative. 600 to 700 ms feels
   much snappier; keep the long value only when the utterance so far is under
   about 1 s (a hesitation) so "um... what time is it" is not cut. One constant
   plus a branch in `Vad::process`; the host VAD test already pins the numbers.
4. **Overlap the next reply with the tail of the current one.** `playTts` waits
   for the speaker to go idle (`MuseVoice.cpp:276`) before the worker may fetch
   the next queued reply, so "working on it" then "here is the result" has an
   audible gap of one full connection setup. After the current stream is fully in
   the ring (it is ahead of the speaker by up to 4 s), start the next fetch.
5. **Gateway.** (a) `DEVICE_RATE_LIMITER` is 10 per 60 s per device and every
   `/v1/speak` spends one: a task flow that announces, then reports, then
   corrects can hit "Voice limit reached". Give `speak` its own, higher limiter;
   cost is already capped by the AI Gateway spend limit. (b) Authentication is a
   D1 query on every request; a 30 to 60 s in-isolate cache of the token hash
   removes it from the hot path (trade-off: a disabled device works for up to
   that long). (c) `speechFrames` forwards each upstream chunk as its own frame;
   fine now, worth coalescing only if logs show tiny chunks.
6. **Small firmware wins.**
   - Done in this change: VAD maths was `double` (software-emulated on the
     S3's single-precision FPU) on every 20 ms block; now integer and `float`.
   - `esp_http_client_read` is called with 1,280 bytes (`TtsStream.h`
     `streamTts`); 4 KiB halves the read calls with the same 3 s timeout.
   - `sdkconfig.defaults` enables `CONFIG_LWIP_IPV4_NAPT` / `IP_FORWARD`, which is
     for soft-AP routing; confirm the Muse SDK does not need them, then drop for
     RAM.
   - SPI is 40 MHz (`Config.h` `kSpiHz`); many ST77xx panels run at 80 MHz. Face
     frames already use dirty tiles, so this only helps full-screen GIFs/JPEGs.

## B. Smoother

- **VAD false triggers and stuck sessions.** The gate is energy only. In steady
  noise above its floor it starts a turn and then cannot end it until the 30 s
  cap, uploading 30 s of noise. Add: if level stays within about 20% of its
  mean for more than 3 s with no energy dips, end and discard. A small tune with
  a host test.
- **No barge-in.** Capture is off during speech (correct without echo
  cancellation). Tap works. Cheaper hands-free options on an S3 with PSRAM:
  ESP-SR WakeNet for a "Hey Bunty" wake word, instead of the first tap, and AEC
  later.
- **Clock before TLS.** `fetchSpeech` does not check that NTP has set the clock
  (`main.cpp` `serviceClockSync` only requests it). A reply before sync fails
  certificate checks as "Speech connection failed". Wait up to a few seconds for
  `time(nullptr) >= cfg::net::kMinimumTrustedUnixTime`, or report a clear error.
- **DNS override.** `serviceClockSync` replaces DHCP DNS with 1.1.1.1/8.8.8.8 on
  every reconnect. That fixes flaky routers but breaks split-horizon or
  filtered networks; prefer DHCP DNS first and fall back only after a failed
  lookup.
- **`ReplyMailbox` is single-producer** (`MuseReplyMailbox.h`). That is why it is
  safe today and why a second source of speech (see C) must go through the voice
  task or a lock, not call `bunty_muse_queue_tts` from another task.

## C. Making it do tasks

What exists: Muse owns the brain (STT, agent, memory, its own integrations) and
Bunty exposes `display.*`, `bunty.look`, `bunty.gesture` and GIF transfer. The
gateway returns Muse's reply as expressive speech. That is the right split: keep
Muse as the brain, and grow **device tools** and **proactive speech**.

Gaps that limit tasks today:

1. **Long tasks lose their result.** Muse is told to announce a slow task and
   deliver the result as a second message, but the SDK link holds a turn for at
   most 60 s (`VOICE.md`). A result after that has no channel back to the speaker.
   Fix with a `bunty.say{text}` command the agent calls at the end of the task. It
   must enqueue through the voice task (single-producer mailbox) and move the
   phase out of `Off` so the face, aura and radio power state are right;
   `power::setRadioAwake` and the 240 MHz CPU request are tied to phase.
2. **No device-side timers or alarms.** Reminders are the most natural task for a
   desk robot and need no cloud: `bunty.timer{op: set|cancel|list, seconds, label}`
   backed by a small fixed table checked in the main loop, firing through the
   same `say` path plus a chime and head gesture. Pure logic, so it can be a
   header with a host test like `speech_player_test.cpp`.
3. **No way to change volume.** `voice_configure_command` returns an error and
   `kVolumePercent` is a compile-time 95. Add `bunty.volume{percent}` persisted in
   NVS, so "turn it down" works.
4. **No state to read.** `display.info` covers memory and reset reason only. A
   `bunty.status` (Wi-Fi RSSI, uptime, volume, sleeping, voice phase, time) lets
   the agent answer "are you ok" and decide when to speak.
5. **Expression without speech.** `bunty.gesture` moves the head but there is no
   `bunty.emote{emotion, seconds}` for the face; the Muse tags only act inside a
   spoken reply. Useful for "working" and "done" indicators during silent tasks.
6. **Show results, not just say them.** With `cfg::ui::kTextContent` false,
   `display.show_text` and `ui.*` are rejected on purpose. For tasks that produce
   a number (timer, temperature, queue length) consider a narrow
   `bunty.card{big_text, caption}` rendered with the existing digit font
   (`Digits24pt.h`) instead of re-enabling all text tools.
7. **Prompting.** The tag format (`[happy] [nod] ...`) and the "announce, then
   result" convention live in the Muse app's instructions, not in this repo.
   Keep a copy of the exact agent prompt in `firmware/MUSE.md` so they cannot
   drift from `speech.js` (`EMOTIONS`, `GESTURES`, `PICTURE_NAMES`).

Suggested order: `bunty.say` (unblocks everything long-running), `bunty.timer`,
`bunty.volume`, `bunty.status`, then `bunty.emote`.

### Gateway-owned chat as an alternative

`iot-gateway` also has a complete STT to Claude to TTS pipeline
(`src/live.js`, 1,500 lines; Flux, Smart Turn, Aura). The current firmware does
not use it: the WebSocket transport was removed in the Muse port. It would give
real streaming STT and model-checked end-of-turn (no 900 ms guess, no
upload-after-pause), and you would own the tool loop. The cost is giving up
Muse's agent, memory and integrations and re-adding the firmware transport. For
"do tasks" I recommend staying on Muse; keep `live.js` for the browser voice UI
and drop it from the device story. If end-of-turn latency matters most, the
Smart Turn idea can still be approximated on-device (item A3).

## D. Housekeeping

- `iot-gateway/README.md` diagram says Aura 2 Aries; `wrangler.jsonc` ships
  `aura-1` / `arcas`.
- `bunty_muse_reply` stores a 1 KiB caption (`Muse.cpp`) that nothing draws now
  that conversation is face-only (`muse::caption()` is declared in `Muse.h` and has no
  caller); remove or reuse for `bunty.card`.
- `VOICE.md` still says some hardware verification is pending; after the first
  successful spoken turn, record measured latencies (end of speech to first
  sound) there. Without numbers, every item above is a guess.

## Verification plan for firmware items

1. `./scripts/idf.sh build`, then host suite `tests/host/run.sh` (needs network
   for `scripts/prepare_deps.py`).
2. Flash without erase. Log timestamps already exist per stage; add one at
   "reply received" and one at "playback started" and compare before and after.
3. Repeat five short and five 15-second questions; compare median latency, and
   confirm no new `[muse-tts] connect failed` lines.
