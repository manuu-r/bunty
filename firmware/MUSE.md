# Muse integration

Bunty uses Muse's ESP32 SDK connection/pairing and low-memory chat transport,
while keeping its existing renderer, microphones, speaker, and servo drivers.
The SDK is pinned to commit `74a5e2d7fc895f109f83a9a1dbed705dbcd8b1ff` and fetched
into ignored `.deps/muse/` from a SHA-256 checked archive. Its Apache 2.0 licence
is in `.deps/muse/LICENSE`; upstream dependency licences remain intact.

## Credentials and build

Get an SDK token from https://gadgets.muse.ai/settings/sdk-tokens. Save just the
`mgst_...` value in `firmware/.muse-token`. This file and the generated header are
ignored; the build script validates the format, writes the header with mode 600,
and does not print the token. The token is compiled into the image, as required
by the community SDK. Do not distribute your firmware binary publicly.

```sh
cd firmware
./scripts/idf.sh build
./scripts/idf.sh -p /dev/cu.usbmodemXXXX flash monitor
```

The wrapper uses `build-muse/sdkconfig`; the old `build/` migration image remains
available. Use a normal flash, without erase-flash. The existing 8 MB partition
geometry is preserved. Muse uses its own `homehub` NVS namespace; Wi-Fi and
account setup come from the Muse app. Old unused NVS records remain inert.
No eFuses, NVS encryption, Secure Boot, tunnel, or OTA are enabled by this port.

## Pairing and interaction

1. Enable Developer mode in Muse's Settings > Devices.
2. Add the device named `MuseGadget-Bunty-...`.
3. When Bunty shows its confirmation screen, tap Yes.
4. Complete Wi-Fi/account provisioning in the phone app.
5. Tap Bunty's mic control once to start a conversation. Local energy VAD detects
   speech, preserves 320 ms of opening audio, and sends after a 900 ms pause.
   Utterances are capped at 30 seconds; quiet waiting opens no Muse request.
6. Muse transcribes the utterance and sends its reply. Captions page on the LCD;
   the gateway's Aura TTS speaks each completed message. Listening resumes after
   the reply automatically. Eight seconds without speech exits the conversation.
   Tap while waiting to exit, during speech to send immediately, or during playback
   to interrupt and listen again.

The interaction is hands-free after the initial tap, but Muse still processes
completed utterances rather than a continuous realtime speech session. Capture
is paused during playback: speech barge-in and acoustic echo cancellation are
not implemented. Energy VAD adapts to quiet background levels; sustained noise
can trigger it, and pause/sensitivity tuning needs verification on Bunty's mics.
The SDK retains at most 1,023 UTF-8 bytes of a message, and bounded queues may
omit older replies in a burst. Full conversation text remains in the Muse app.
Speaker faults fall back to captions/app text; they don't invoke another LLM.

Muse owns the Wi-Fi and NimBLE lifetime. Gauge commissioning, discovery,
dashboard/cache, and Wi-Fi recovery code have been removed, along with the
legacy firmware WebSocket voice transport. The platform Wi-Fi facade cannot
initialize or reconnect the driver. Idle UI is Bunty's animated face with
Settings/microphone controls. Settings unpair resets Muse setup and reboots;
the gateway TTS credential and servo calibration survive.

Fresh phone provisioning restarts once after storing credentials. This uses
Muse's existing display-board safeguard: BLE provisioning and TLS connection
must not compete for the same internal DMA heap. Voice capture starts after
that restart. CPU/radio power changes are applied by the main task, with worker
requests passed atomically. Boot logs include the previous reset reason; platform
initialization errors halt with a diagnostic while retaining NVS.

## Gateway TTS

`POST /v1/tts` accepts `{"text":"Muse's reply"}` with Bunty's existing device
bearer token. It uses the existing Aura-1 `arcas` voice through Workers AI, with
no STT or Claude invocation. Device auth, request rate limiting, and the existing
text/body bounds apply. The response is raw mono 16 kHz little-endian PCM,
labelled `audio/l16;rate=16000;channels=1` and `X-Audio-Byte-Order: little-endian`.
This header is a description of the gateway's existing PCM convention, not a
claim of the network-endian RFC audio/L16 encoding. Firmware checks the response
status/type, handles odd transport boundaries, and rejects incomplete audio.
The browser `/v1/voice/speak` and legacy `/v1/live` remain available.
Firmware now speaks through `POST /v1/speak`, which adds expression cues to this
PCM; see [VOICE.md](VOICE.md#one-clock-for-sound-face-and-head).

Deploy from `iot-gateway/` with `npm run deploy`. No new TTS provider key is
needed: the existing Workers AI binding and gateway credential are reused.

## Remote commands

The capabilities are included in Muse's `commands_v2` registration. With
`cfg::ui::kTextContent` false, `display.show_text` and every `ui.*` tool are
withheld and rejected, so Muse speaks that content instead:

| Command | Behavior |
| --- | --- |
| `display.draw_url` | Fetch and display a baseline JPEG, animated GIF, or big-endian RGB565 image from HTTP(S) |
| `display.show_text` | Wrapped, paged text with an optional title and colours |
| `display.show_gif` | Fast one-call fullscreen GIF display: only data_base64 required, max 6 KiB, no session metadata or checksum |
| `bunty.gif_transfer` | Receive a native GIF in bounded base64 chunks; CRC32-checked commit, progress and cancellation (see [GIF.md](GIF.md#direct-file-transfer)) |
| `display.info` | Screen dimensions/formats, download state, memory, and previous reset reason |
| `display.show_animation` | Dismiss remote content and restore Bunty's face |
| `bunty.look` | Queue yaw/pitch movement |

The image area is the full rectangular **240 × 320** LCD, 16-bit RGB565. JPEGs
are centred and shrunk by 1/2, 1/4, or 1/8 as needed. Progressive JPEG is rejected;
images too large even at 1/8 are rejected. RGB565 input is high byte first,
480 bytes per row, at most 320 rows. `row` is an optional integer from -1 to 319:
-1 centres JPEGs, zooms GIFs edge-to-edge with centred cropping and starts raw data at 0; another value sets the top row at native GIF size.
Raw data must fit in the remaining rows. GIF87a/GIF89a animations are decoded
natively, with a maximum 240x320 canvas, 1 MiB file and 512 frames. GIFs are
zoomed to fill the screen by default, with aspect ratio preserved and centred cropping. Explicit `row` values use native-size placement; resize larger GIFs before
sending. Frame delays are clamped to 100–10,000 ms (at most 10 fps), file loop
counts are respected, and the final frame remains visible for finite animations.
Transparency, local palettes and disposal methods 0–3 are supported. GIF plain
text extensions are rejected. A malformed later frame stops on the last valid
screen. Convert PNG, WebP, SVG and other formats to baseline JPEG or RGB565.
Video codecs are not supported. See [GIF.md](GIF.md) for memory and verification.

Image URLs are bounded to 2,048 bytes. Downloads follow at most three redirects,
keep the original HTTP/HTTPS scheme, and have a 60-second deadline and heap
checks. A separate 153,600-byte PSRAM buffer holds incoming pixels: failed,
empty, oversized, partial or dismissed downloads never replace the current
screen. Image and TTS HTTPS work are serialized to avoid competing handshakes.
Image commands report `busy` during speech downloads; TTS waits for an image
download and remains tap-interruptible. A tap during download cancels display;
the fetch stops on its next read/draw. A displayed image stays until a tap or
`display.show_animation`. Voice turns may continue while content is displayed,
but face/caption animation does not overwrite it.

`display.show_text` requires `text` (1–4,096 UTF-8 bytes). Optional `title` is
at most 96 bytes; `foreground` and `background` are `#RRGGBB`. It wraps using
actual font metrics, preserves line breaks, and pages long text on taps. The
last tap restores the face. The bundled font is ASCII; unsupported code points
become `?`. Render multilingual text, emoji, charts, or rich layouts as an
image for full visual fidelity. Text commands report `busy` during image fetches.

`bunty.look` accepts required numeric `yaw` and `pitch` parameters, each between
-40 and 40 degrees relative to the calibrated home. The acknowledgement means
queued, not motion completed. Commands are applied on the main task, with one
axis moving at a time under the existing power claims; the latest queued target
replaces an older one. Actual movement waits when the power policy refuses it.
Bunty does not report measured battery voltage or percentage because there is
no battery telemetry hardware.

## Verification

Native ESP-IDF build, existing host checks, provisioning tests, and the gateway
suite are run during integration. Pairing, remote commands, voice upload,
caption rendering, TTS playback/cancellation and reconnect must also be verified
on the connected device with a real Muse token/account. A passing build alone
is not proof of an authenticated Muse session.

Current integration checks: native firmware build passed (54% app slot free),
existing host checks and four provisioning tests passed, all 47 gateway tests
passed, and six upstream caption wrapping tests passed. The TTS route is deployed;
a live authenticated call returned 57,586 bytes of mono 16 kHz PCM with the
expected response headers. The initial Muse firmware was flashed successfully;
authenticated Muse pairing remains pending a real SDK token. VAD onset, pauses,
duration limits, and pre-roll ordering also have host checks; actual room audio
and conversation turn-taking still need verification with that token.

The Muse-only cleanup/VAD/image/text firmware was flashed successfully to Bunty
on `/dev/cu.usbmodem101`. A 40-second serial check showed one initial USB reset,
normal display/touch/servo initialization, Muse BLE advertising, and no panic or
repeated resets. At 30 seconds, internal free heap was 95,339 bytes and the
main task had 6,876 bytes of stack headroom. This verifies unpaired startup;
paired operation still needs a real token/account. The cause of the previously
reported reset loop is not established without its original panic/reset log.
Static review removed a Gauge recovery path that could create a duplicate
Wi-Fi netif and enabled the SDK's low-memory provisioning safeguard for Bunty.

Display checks cover rectangle bounds, RGB565 byte order, cancellation/stale
writes, ASCII/UTF-8 fallback, font-metric wrapping and paging. A host harness
compiles the actual adapted SDK downloader and checks fragmented raw transfer,
failed/empty/oversized downloads, cancellation/retry, redirects and deadline
handling. JPEG tests exercise actual sizing/placement/RGB565 callbacks with a
stub for the ESP32 ROM decoder. Real JPEG decoding, LCD rendering and command
delivery through an authenticated Muse session remain hardware checks.

### Reply-path refactor (2026-10-05)

Muse polling/capture now stays on `muse_voice`; a separate `muse_tts` worker
owns HTTP, speaker writes and cleanup. Its 12 KB internal-RAM stack is independent
of the 8 KB voice stack. A four-slot SPSC mailbox and PCM scratch buffers are
allocated once in PSRAM. No reply-sized temporary is copied onto the callback
stack. The active slot remains owned until playback and HTTP cleanup finish.
Cancellation invalidates queued jobs by generation; microphone capture waits
until all old jobs are released. Muse DONE also waits for playback before VAD
resumes. Speaking animation starts after the speaker opens, not during TLS.

Disassembly of the previous build showed 1,920 bytes for `Voice::run` plus
3,072 bytes for its nested `playTts`, before HTTP/TLS callees. After refactoring,
`run` uses 912 bytes and `playTts` approximately 544 bytes on separate tasks.
This establishes stack pressure in the old design, **not a confirmed reset cause**.
The new logs report TTS stages, HTTP status and stack headroom without reply text
or credentials. Host tests cover cancellation with an active slot, bounded
capacity, truncation and 50,000 concurrent producer/consumer handoffs.

The refactored firmware builds and the host suite passes. No USB device was
visible during this change, so this build has not been flashed or validated with
a live Muse reply. Capture the boot reset reason/panic backtrace and exercise
speech, interruption and repeated replies on hardware before declaring the
reported reset fixed.

The wider runtime refactor is documented in [RUNTIME.md](RUNTIME.md). It adds
batched LCD DMA, nonblocking UI transitions, capture-session isolation, bounded
TTS JSON and checked Muse queue/mutex initialization. Live validation remains
pending a USB connection.

2026-10-06: flashed the runtime refactor successfully (hash verified). The
post-flash serial capture shows provisioned Wi-Fi/Muse connections and no
repeated reset during idle observation. Live reply playback remains to be
verified; see [RUNTIME.md](RUNTIME.md) for measured heap/stack headroom.

## Structured screens and speech gestures

Seven `ui.*` tools provide calendar/task/note snapshots, information cards,
navigation, timezone context and state reads. The default idle home is the animated face;
Recent contains requested content only. Tap the face to talk. Every voice upload now includes
instructions asking Muse to open each sentence with an expression tag instead of
calling `bunty.gesture`, which remains for silence and images. The
device executes small named gestures through the conversation controller,
and queues manual `bunty.look` movement until that controller releases the head.
See [UI.md](UI.md) for schemas, pending-task acknowledgment and RAM-only storage.
2026-10-06: flashed the UI/gesture build with hash verification. The expanded
8,892-byte registry required a Bunty-only 32 KB PSRAM serialization buffer;
Muse acknowledged registration after that fix. Live reply/gesture timing remains
to be verified.

## Reply playback and listening controls

See [VOICE.md](VOICE.md) for the corrected TTS failure flow, direct PCM header
validation and the [modern listening preview](docs/bunty-listening-preview.png).
End cancels, Send now ends detected speech, and pausing still sends automatically.
The gateway was verified from this Mac; physical reply playback remains pending
USB availability and flashing.

Bunty downloads image/GIF URLs directly. The tool description and voice request
now tell Muse to use an existing fitting direct URL and avoid unnecessarily
re-uploading/recreating it. Structured text should use UI/text tools. Any actual
Muse-side artifact hosting approval remains controlled by the app; this guidance
is not a way to approve or bypass it. SDK checks and the ESP-IDF build pass.

2026-10-06 follow-up: a 180-second filtered serial capture recorded startup and
Muse registration but no voice attempt, so the reported thinking failure and
image-hosting prompt were not reproduced. The exact error/prompt text and a
live failing turn are still needed; do not claim this guidance fixes that failure.
The direct-URL guidance was flashed with checksum verification; startup
confirmed Muse registration. No voice attempt was captured after this flash.


Meme reactions: `display.show_gif` and `display.draw_url` accept optional
`gesture` and `speed` in the same call. Successful display triggers a reaction
without waiting for speech. Fast media reaction is the default (1,300 ms); normal is 1,800 ms and
slow is 2,400 ms. Media uses wider held poses, while voice remains gentle. Standalone `bunty.gesture` also accepts `when` (auto/speech/display/now).
See [GIF.md](GIF.md#meme-reactions-and-tempo) for timing, cancellation and limits.
