# Animated GIF playback

Muse can use the existing `display.draw_url` command with a GIF URL:

```json
{"url":"https://example.com/animation.gif","row":-1}
```

Format is detected from the downloaded bytes, so a filename extension or content
type is not required. GIF87a/GIF89a canvases must fit 240x320; files are bounded
to 1 MiB (1,048,576 bytes) and 512 frames. Resize oversized GIFs before sending. The optional
row is -1 by default: zoom edge-to-edge to fill the 240x320 panel, preserving
aspect ratio with centred cropping. Explicit rows 0–319 retain native-size
placement vertically, centred horizontally. Playback respects file loop
counts, transparency, local/global palettes, interlacing and disposal 0–3.
Frame delays are limited to 100–10,000 ms, measured after presentation, so
playback runs at most 10 fps and leaves time for voice and touch work. Plain text
extensions and unsupported disposal values are rejected.

The full file is downloaded and structurally checked before its first frame is
decoded and committed. Download/first-frame failures preserve the previous screen.
A later decode error stops on the last good frame. A finite animation holds its
last frame. Tap, `display.show_animation`, a UI view or replacement content stops
playback; the listening aura remains composed independently on every frame.

The decoder is the checksum-pinned [AnimatedGIF library](https://github.com/bitbank2/AnimatedGIF),
kept in `.deps/`, with its build definition under `lib/animatedgif`. Dependency
preparation ensures global/local palette reads include bounded frame-header
lookahead. The previous palette-only patch rejected valid 256-colour global
palettes because it left the following frame header unread. Regression checks
cover both 256-entry global and local palettes and small files. Data after the
GIF trailer is ignored within the file-size cap. Decoder error codes are checked
even if some rows were produced, so a partial decode never becomes a successful
first-frame commit. Rejection diagnostics distinguish structure, placement,
opening and first-frame decoding failures.
The decoder uses raw rows; Bunty supplies its own RGB565 canvas and a restore
snapshot for disposal 3. File, decoder and both canvases live in PSRAM. The
compressed buffer starts at 64 KiB, grows only when full (up to 1 MiB), and
shrinks to the downloaded size when possible. The decoder and canvases add
roughly 325 KiB, including precomputed scaling maps. Replacing a maximum-sized animation temporarily retains both
(about 2.65 MiB), with up to another 512 KiB during a moving buffer growth.
Growth admission budgets the whole new allocation plus both canvases and a
1 MiB PSRAM reserve for other work; no fallback to internal RAM is permitted.
Insufficient memory rejects the candidate and keeps the previous display.
Structurally invalid files are rejected before allocating canvases. Exact-limit
files are accepted, while an extra-byte probe rejects larger files. Fragmented
short reads are accumulated until EOF; buffers remain owned on every failure.
Buffers are freed
when playback finishes, fails, is dismissed or replaced. The first GIF allocates
a checked 6,144-byte worker stack in internal RAM; that worker parks on a task
notification when idle. Frame decoding/presentation allocate no heap storage.

The low-priority worker advances at most one frame per wake. It serializes with
other display operations through the UI mutex; the voice task and TTS worker
remain independent. Display transfers use the existing 3,840-byte DMA buffer.
The HTTP downloader retains its timeout, 60-second deadline, scheme-preserving
redirects, network lease, cancellation and heap admission checks. HTTPS is only
needed for downloading; GIF playback releases that network lease so spoken
responses can continue while the animation is displayed.

Host checks run the actual pinned decoder against generated GIFs and assert
pixel results for transparency, disposal 2/3, local palettes, interlacing, finite
and infinite loops, delay limits, clock wrap, dimensions, frame limits, exact 1 MiB and over-limit files, short reads,
allocation/read failure and truncation. SDK downloader checks verify GIF sniffing, fragmented reads,
successful commit and failed decode rollback alongside JPEG/RGB565 cases.
Physical animation and concurrent speaker playback still need verification on
Bunty.

The face-only UI/GIF update built and flashed successfully. Startup verification
confirmed Muse registration, no observed panic, and about 6.2 MiB free PSRAM
before GIF playback. This does not verify a live animation or spoken reply.

The palette-lookahead follow-up passed tests, built and was flashed successfully
with checksum verification. The reported GIF URL is still needed to confirm
that the exact file succeeds.

## Direct file transfer

The preferred direct path is `display.show_gif`. Supply only `data_base64`:

```json
{"data_base64":"<base64 of GIF bytes>"}
```

Files up to 6,144 decoded bytes display in one call. There is no transfer ID,
byte count, CRC32, begin/chunk/commit sequence, device confirmation or preliminary
`display.info` call. Optional `row` retains the placement behavior described
above. Bunty decodes directly into PSRAM and displays full screen. Size, base64,
structural and decoder checks remain to prevent memory overruns or resets;
invalid candidates keep the previous display. Retrying this simple command
restarts playback. Muse needs access to the actual file and a way to base64-encode
it; this endpoint does not change app permissions or model/tool execution time.

`bunty.gif_transfer` remains a compatibility fallback for larger files. Its
metadata-bearing `op: "show"` also remains supported for existing clients.

For larger files prefer an existing usable URL with `display.draw_url`. The
HTTP download avoids repeated model/tool calls. When no usable URL exists,
invoke these chunk-transfer calls:

1. `begin`: `id`, exact `bytes`, eight-hex-digit standard `crc32`, optional `row`.
2. `chunk`: same `id`, decoded-byte `offset`, canonical `data_base64` containing
   at most 6,144 decoded bytes (8,192 characters). Send calls sequentially and
   check `received_bytes` before continuing.
3. `commit`: same `id`. Success reports `displayed: true` only after checksum,
   structural validation and first-frame decode succeed.

`status` reads progress; `cancel` frees an active transfer. IDs contain 1–32 ASCII
letters, digits, hyphens or underscores. Begin and identical chunk retries are
idempotent. Conflicting bytes, gaps, noncanonical base64 and out-of-bounds offsets
are rejected without altering progress. A successful commit is remembered for
five minutes (or until the next begin), so retrying a lost acknowledgement does
not restart the animation. Use a new ID for each new file. Checksum failure or
invalid GIF at commit discards the candidate and preserves the previous screen.
Incomplete commits retain progress so the sender can finish the file.

Only one transfer is admitted, capped at 1 MiB. Its exact-sized file buffer lives
in PSRAM and is moved into playback without copying it into a second file buffer.
Admission reserves space for the decoder/canvases plus 1 MiB for voice/content.
The buffer is freed after 30 seconds without new bytes, or after five minutes
regardless of progress. Status and duplicate retries do not extend these limits.
Display dismissal cancels pending transfers. Receiving chunks does not claim the
HTTP/TTS network lease or stop the current display. A valid commit replaces the
screen. Native GIF limits and playback behavior remain as above.

Many tool calls make large files slow; prefer optimized files under 64 KiB.
Muse must have access to the actual file bytes and a means of encoding them.
Firmware support does not add a file-reading capability to Muse or bypass its
permissions. If it cannot send bytes, the direct-URL path remains available.

Create exact command/parameter pairs for a local file:

```sh
python3 scripts/pack_gif.py animation.gif > /tmp/gif-calls.jsonl
```

The packer checks the file size, signature and geometry; device commit performs
full structural/decode validation. The script automatically emits data-only `display.show_gif` for small GIFs and chunks for larger ones.
It emits calls; it does not connect to
Muse, upload the file or optimize it. `docs/direct-gif-demo.gif` is a small looping
64x64 face, with a ready-made one-call sequence in
`docs/direct-gif-demo.jsonl`. Have Muse invoke each command with the supplied
parameters in order, stopping on any failed acknowledgement. Its fixed demo ID
is for one run; regenerate with the packer for repeated runs.

Host verification covers exact byte reconstruction, checksum vectors, strict
base64, maximum chunks, a 1 MiB transfer, duplicates, ordering/conflicts, timeouts
and clock wrap. A GIF assembled from chunks is exercised with the actual decoder.
Muse-side file access and a physical direct-transfer animation still need a live
verification.

The direct-transfer firmware passed host/Python checks and the ESP-IDF build,
then was flashed with written-image checksum verification. Post-flash startup
confirmed Muse registration. This verifies boot and command registration, not
Muse file-byte access or a physical direct-transfer animation.

The speed follow-up doubles chunks from 3 KiB to 6 KiB within the existing
12 KiB inbound frame scratch. The demo's lossless LZW encoding was reduced from
6,257 to 4,715 bytes without changing its canvas, colours, pixel results or
frame timing, and its transfer now requires one call instead of five. This is
a reduction in protocol round trips, not a measured Muse latency improvement;
file access, base64 generation and actual tool execution remain app-dependent.
The speed follow-up passed all host checks and ten Python tests, built and was
flashed with checksum verification. Startup confirmed Muse registration and
unchanged free PSRAM (6,560,308 bytes before any GIF transfer).

Fullscreen GIF zoom applies equally to direct files and URL downloads. Disposal
and transparency are composed at native size before nearest-neighbour sampling.
A 64x64 GIF fills the screen as a 320x320 image with 40 pixels cropped from each
side. This preserves proportions and file size. Scaling maps are computed once
per file in PSRAM; presentation uses one 480-byte stack row, with no additional
framebuffer or per-frame heap allocations. Tests verify enlarged pixel placement,
full-screen edge coverage, centre cropping of wide/tall files, a 1x1 source,
animated blinking/disposal and explicit-row native rendering.
The fullscreen zoom update passed the host decoder/render checks and all ten
Python tests, built without warnings and was flashed with checksum verification.
Post-flash startup confirmed Muse registration; physical animation appearance
remains for device verification.

The simplified path omits CRC computation and transfer-session bookkeeping
entirely. Base64 is decoded in one pass into the playback-owned file buffer.
Tests cover metadata-free encoding/decoding, exact 6 KiB payloads, over-limit
and malformed base64, real GIF decoding and fullscreen pixel output. The legacy
chunk checks still apply only to that compatibility path.
The simplified data-only endpoint passed host checks and all ten Python tests,
built without warnings and was flashed with checksum verification. Startup
confirmed Muse registration and no observed panic. Live Muse transfer latency
and animation appearance still need verification on the device.


## Meme reactions and tempo

Both `display.show_gif` and `display.draw_url` accept optional `gesture` and
`speed` in the same call. For example, add `"gesture":"happy","speed":"fast"`
to the normal GIF parameters. No separate movement call or speech is required.
The reaction is queued only after a successful display; failed or dismissed
files do not execute their attached reaction. The direct demo includes a fast
happy reaction. Muse is instructed to select a gesture and tempo from the meme's
context, with fast preferred for energetic content.

Gestures are `neutral`, `nod`, `shake`, `curious`, `happy`. Tempo presets are
`fast` (default, 1,300 ms), `normal` (1,800 ms), `slow` (2,400 ms). The 20 ms motion
controller executes one finite small-arc reaction, then returns to the current
voice pose or home. Media poses reach up to 18 degrees yaw and 15 degrees pitch;
follow rates are 60/50/35 degrees per second respectively, with existing power
limits taking precedence. GIF frame delays are unchanged: tempo is chosen by
Muse to match the scene, rather than tracking individual GIF frame timecodes.

Standalone `bunty.gesture` accepts `speed` and `when`: `auto` (default) starts
with the next speech or picture, or immediately if a picture is already shown;
`speech` waits for speech; `display` waits for the next successful image display;
`now` starts immediately even in silence. A queued auto gesture is consumed by
the picture so it does not repeat on speech. Explicit speech gestures stay with
speech. Voice listening resumes without cancelling media reactions. Dismissing
an image or ending the conversation cancels pending/active visual reactions.
There are no dynamic motion allocations or SDK-thread servo writes. Commands
publish to bounded atomic mailboxes; the main loop arbitrates servo ownership
and power before the independent servo tasks move.
The meme reaction update passed host checks and all eleven Python tests, built
without warnings and was flashed with written-image checksum verification.
Startup confirmed Muse registration and no observed panic. Physical reaction
appearance and timing still need a live meme request on Bunty.


The visibility follow-up replaces the small sinusoidal media sway with held
poses. Happy alternates +/-16 degrees yaw and +/-14 pitch; nod uses +/-15 pitch,
shake uses +/-18 yaw, and curious holds +18 yaw/+15 pitch before returning home.
The unchanged follower smooths/rate-caps the transitions, so the target changes
do not become instantaneous servo jumps. Fast media reactions now take 1.3 s,
normal 1.8 s and slow 2.4 s, giving the servo time to reach a visible angle.
Voice gestures retain their previous gentle amplitude and 0.9/1.4/2 s timings.
Servo pulse calibration, maximum offsets, power admission and rate caps remain
unchanged. The motor follower is factored into ServoFollow.h and tested against
the actual media profiles: it must reach substantial positive/negative angles,
remain rate bounded and return home at each tempo. Motion logs identify selected
reaction, tempo, axes and power mode, or report missing image gesture parameters.
The visibility update passed all host checks (including the actual follower
with the wider trajectories) and eleven Python tests, built without warnings,
and was flashed with checksum verification. Startup confirmed both servo GPIOs
initialized, Muse registration and no observed panic. No fresh meme request was
captured during this startup check; physical motion visibility remains to verify.
