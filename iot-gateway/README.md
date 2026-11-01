# IoT Gateway

A narrow Cloudflare Worker API for sending bounded IoT requests to Claude without placing an Anthropic or Cloudflare AI Gateway credential on any device.

```text
IoT device -- bearer token + PCM WebSocket --> Worker --> AI Gateway
                                                   |          |
                                                   |          +-- Flux live STT
                                                   |          +-- Claude Haiku
                                                   |          +-- Aura 2 Aries TTS
                                                   +-- D1 device registry
                                                   +-- per-device rate limits
```

The public API deliberately does **not** accept a model, system prompt, tools, provider headers, or raw Anthropic request body. The Worker chooses those values and caps request size and output tokens.

## What is included

- `POST /v1/infer` with a small JSON schema
- `GET /v1/live` WebSocket for raw PCM → Flux → Claude → Aura Aries → raw PCM
- `POST /v1/speak`: tagged reply text → Aura PCM framed with expression cues and tiny reaction GIFs, for Muse-driven devices
- Password-protected Bunty browser voice UI at `/`, using Flux STT and Aura TTS
- Per-device random bearer tokens; only SHA-256 hashes are stored in D1
- Individual device enable/disable and token rotation
- Per-device Cloudflare Workers rate limiting
- AI Gateway metadata for per-device spend limits
- Anthropic BYOK through the pre-authenticated Workers AI binding
- Prompt logging disabled by default
- Local tests that require no cloud account

## 1. Install and test

Requires Node.js 20 or newer.

```sh
npm install
npm test
```

## 2. Create the AI Gateway

In the Cloudflare dashboard:

1. Open **AI > AI Gateway**.
2. Create a gateway with the ID `iot-gateway`.
3. Enable authenticated gateway access.
4. Under **Provider Keys**, add the Anthropic API key using the `default` alias.

The key is then stored by Cloudflare. It is not added to this project. If you prefer Cloudflare Unified Billing and the selected model is available there, you can omit the Anthropic key.

The Worker uses an AI binding, which Cloudflare pre-authenticates within the same account. No AI Gateway bearer token is required in the Worker.

## 3. Create D1 and apply the schema

Authenticate Wrangler, create the database, and copy the returned `database_id` into `wrangler.jsonc`:

```sh
npx wrangler login
npm run db:create
```

Replace this placeholder:

```json
"database_id": "REPLACE_WITH_YOUR_D1_DATABASE_ID"
```

Apply the migration:

```sh
npm run db:migrate:remote
```

For local development, also run:

```sh
npm run db:migrate:local
```

## 4. Review the configuration

Edit `wrangler.jsonc` before deploying:

- `AI_GATEWAY_ID`: your AI Gateway ID.
- `AI_MODEL`: a currently available Cloudflare model ID such as `anthropic/claude-haiku-4.5`.
- `STT_MODEL`: `@cf/deepgram/flux`, Cloudflare's live conversational speech-recognition model.
- `STT_EAGER_EOT_THRESHOLD`: `0.7`; enables Flux's early pause and resumed-speech events.
- `TURN_DETECTION_MODEL`: `@cf/pipecat-ai/smart-turn-v2`; verifies a Flux pause using the recent PCM waveform before the Worker ends a turn. `TURN_DETECTION_THRESHOLD` is `0.65`, the minimum completion confidence, and `TURN_DETECTION_FALLBACK_MS` is `1500`, the maximum extra quiet period before the Worker finalizes a turn. `TURN_DETECTION_AUDIO_SECONDS` caps the retained waveform at 12 seconds.
- `TTS_MODEL`: `@cf/deepgram/aura-1` with `TTS_SPEAKER` set to `arcas`.
- `SYSTEM_PROMPT`: the trusted instruction applied to every text request.
- `VOICE_SYSTEM_PROMPT`: currently asks Claude for a concise, spoken reply of one or two sentences.
- `VOICE_MAX_OUTPUT_TOKENS`: `512`; the voice reply ceiling, also bounded by the device row and `GLOBAL_MAX_OUTPUT_TOKENS`. Migration `0003` lifts the default device cap to 512.
- `AI_GATEWAY_LOGS`: `false` by default so prompts are not retained in Gateway logs.
- `LIVE_HISTORY_TURNS`: `6`; recent voice turns replayed to Claude per session. `0` disables conversation memory.
- `LIVE_AUDIO_SAMPLE_RATE`: `16000`; both directions use signed 16-bit mono PCM.
- Live session duration, audio, frame, turn, and playback-ack limits.
- Token and text limits: global hard limits enforced in addition to each device's limits.
- `namespace_id`: change `1001` or `1002` if either rate-limit namespace is already used elsewhere in your Cloudflare account.

Cloudflare names the requested voice as a model/speaker pair rather than one
literal model ID: `aura-1-arcas` means `@cf/deepgram/aura-1` plus
`speaker: "arcas"`. Flux and Aura are Cloudflare-hosted Workers AI models, so
they do not require an additional Deepgram API key. Your Anthropic
key remains in AI Gateway as described above.

Each voice utterance is its own session. Current Bunty firmware admits speech
locally, then requests gateway-owned endpoint detection using
`X-Bunty-Turn-Detection: gateway`. The authenticated device route enables
Flux/Smart Turn for that connection and advertises `turn_detection: smart-turn-v2`
in `session.ready`. The Worker forwards `ForceEndTurn` followed by `CloseStream`
after its endpoint decision, then runs Claude/Aura and closes the device socket
after playback is acknowledged. `input.ended` tells the firmware to stop PCM
immediately; candidate/incomplete/resumed events do not end input.

The device closes unproductive input with `session.close` after four seconds
without sustained local speech or evolving gateway transcript text. That is
cancellation, not an `input.end` request to generate a reply. Deploy this
gateway before the new firmware, which rejects a missing turn-detection
capability. Legacy clients without the header retain device-owned `input.end`.

The configured limits allow twenty live-session openings and ten finalized
voice turns or text requests per device per 60 seconds. Cloudflare's Workers Rate
Limiting API is intentionally eventually consistent and local to a Cloudflare
location; configure an AI Gateway spend limit as the final cost backstop.

## 5. Provision a device

Create or rotate a device token:

```sh
npm run device -- provision kitchen-sensor-01 --remote
```

The token is displayed once. Store it in the device's secure storage. Running `provision` again for the same ID immediately replaces the stored hash, invalidating the old token.

List or disable devices:

```sh
npm run device -- list --remote
npm run device -- disable kitchen-sensor-01 --remote
npm run device -- enable kitchen-sensor-01 --remote
```

For local D1, substitute `--local` for `--remote`.

### How `--remote` reaches D1

The device command runs the project's local Wrangler executable as:

```sh
wrangler d1 execute iot-gateway-db --remote --command "..."
```

Wrangler reads the `database_id` from `wrangler.jsonc`, obtains your Cloudflare
account authorization from `wrangler login` (or `CLOUDFLARE_API_TOKEN` and
`CLOUDFLARE_ACCOUNT_ID`), and sends the SQL operation to Cloudflare's D1 API over HTTPS. The
Worker does not need to be running locally or already deployed. Without
`--remote`, Wrangler instead changes the emulated database under `.wrangler/`.

You can verify which Cloudflare account the local CLI will use with:

```sh
npx wrangler whoami
```

### Safe firmware rotation

When `BUNTY_PROVISION_IOT=1` is set for a Bunty PlatformIO upload, the hook uses
the internal `stage` and `activate` commands. Staging adds a pending token
without invalidating the active one; authentication accepts either hash. A
successful flash activates the pending token and retires the old one. This
prevents a failed compile or upload from locking the existing firmware out.
Ordinary uploads do not contact Cloudflare and preserve the device's existing
NVS credential.

## 6. Run and deploy

### Browser voice UI

Opening the Worker root (`/`) provides a small browser voice interface. The
browser acts as the edge device: it captures microphone audio, converts it to
16 kHz linear PCM, and streams it to `/v1/voice/live`. That route runs the same
Flux STT → Claude → Aura TTS pipeline as Bunty's `/v1/live` device route, then
returns Aura's MP3 audio for local playback. No device bearer token is exposed
to the browser; the signed UI session cookie authenticates the WebSocket.

For the browser route and opted-in devices, Flux's `EagerEndOfTurn` signal asks Smart Turn v2
to classify the retained PCM window as complete or incomplete. A confident
complete result immediately sends `ForceEndTurn`/`CloseStream` to Flux; resumed speech cancels
the pending decision, and a 1.5 s quiet-period fallback keeps a false incomplete
result from leaving the mic open. The microphone button can always end a turn
immediately. A non-eager `EndOfTurn` also starts verification if none is pending.
Only legacy physical-device clients retain firmware-owned endpoints.
Typing remains a fallback and uses the short text path plus the same Aura TTS
voice as the live Bunty pipeline.

The UI has one shared password. For local development it is read from `.env`:

```sh
cp .env.example .env
# Edit UI_PASSWORD in .env
```

For deployment, `.env` is deliberately not uploaded. Set the same value as a
Cloudflare Worker secret before deploying:

```sh
npx wrangler secret put UI_PASSWORD
```

The Worker sets a signed, HTTP-only, same-site session cookie that expires after
12 hours. Use a long unique password and rotate it by updating the secret (and
your local `.env`); old sessions immediately stop working. The page is not a
device-control channel—it is a voice front end for Bunty's trusted assistant
prompt, so it can converse about the setup without exposing a device token to
the browser.

```sh
npm run dev
npm run deploy
```

Health check:

```sh
curl https://YOUR-WORKER.workers.dev/health
```

Inference request:

```sh
curl https://YOUR-WORKER.workers.dev/v1/infer \
  --request POST \
  --header "Authorization: Bearer YOUR_DEVICE_TOKEN" \
  --header "Content-Type: application/json" \
  --data '{
    "request_id": "reading:123",
    "input": "Explain whether this reading needs attention in one sentence.",
    "context": {
      "temperature_c": 48.2,
      "battery_percent": 71
    },
    "max_output_tokens": 100
  }'
```

Request fields:

| Field | Required | Constraint |
| --- | --- | --- |
| `input` | Yes | Non-empty string, capped per device |
| `context` | No | JSON object; treated as untrusted data |
| `max_output_tokens` | No | Positive integer below device and global caps |
| `request_id` | No | 1-64 safe identifier characters |

Example response:

```json
{
  "request_id": "reading:123",
  "result": {
    "content": [
      {
        "type": "text",
        "text": "The temperature is elevated and should be checked promptly."
      }
    ]
  }
}
```

### Live voice WebSocket

Open an authenticated WebSocket connection—there is no audio-file upload:

```text
wss://YOUR-WORKER.workers.dev/v1/live
Authorization: Bearer YOUR_DEVICE_TOKEN
X-Request-ID: bunty:live:123        # optional
```

The connection runs this fixed, half-duplex pipeline:

```text
binary PCM frames
  -> @cf/deepgram/flux live WebSocket
  -> Flux EndOfTurn
  -> anthropic/claude-haiku-4.5
  -> @cf/deepgram/aura-1 (speaker: arcas, MP3)
  -> binary PCM frames on the same device WebSocket
```

Input frames must be raw signed little-endian 16-bit mono PCM at 16 kHz. Do not
add a WAV header and do not base64-encode the data. Send 80 ms frames (2,560
bytes at this format), which is Flux's recommended chunk size. The gateway
forwards frames as they arrive rather than retaining a complete recording.

Client-to-gateway messages:

| Message | Format | Purpose |
| --- | --- | --- |
| Microphone audio | Binary | Raw `linear16` PCM frames |
| `ping` | `{"type":"ping"}` | Application heartbeat; receives `pong` |
| Playback complete | `{"type":"playback.finished","turn":"3"}` | Confirms the I2S output buffer has fully drained |
| Close | `{"type":"session.close"}` | Gracefully ends the session |

Gateway-to-client JSON controls:

| Type | Device behavior |
| --- | --- |
| `session.ready` | Verify the negotiated input/output audio format |
| `transcript.partial` / `transcript.final` | Optional UI or serial diagnostics |
| `input.pause` | Stop sending microphone data before speaker playback |
| `assistant.thinking` / `assistant.text` | Optional status and diagnostics |
| `audio.start` | Prepare the I2S output path for raw PCM |
| Binary messages | Decode and play the returned MP3 audio |
| `audio.end` | Drain the playback buffer, then send `playback.finished` |
| `input.resume` | Resume microphone streaming |
| `error` | Inspect `code`; apply `retry_after` when present |

The playback acknowledgment is important: the gateway keeps microphone input
paused after `audio.end` until the device confirms its speaker buffer is empty.
This prevents Bunty from transcribing its own voice. A 30-second fallback
resumes input if the acknowledgment is lost.

When the device sends an `X-Bunty-Session` header and `LIVE_HISTORY_TURNS` is
above `0`, the gateway prepends the last N user/assistant turns for that
`(device_id, session_id)` pair to the Claude request. History is stored in the
D1 database from migration `0002_conversation.sql` and is capped at
`LIVE_HISTORY_TURNS` turns per session. The device mints `session_id` once per
provisioned flash, so memory persists across the reconnect forced at
`LIVE_MAX_SESSION_SECONDS` but resets on the next reflash; a new `session_id`
for a device deletes that device's older rows. Set `LIVE_HISTORY_TURNS` to `0`,
or omit the header, to keep every turn independent. All history reads and
writes are best-effort: a D1 error downgrades that turn to stateless rather
than failing it.

The gateway does not write PCM, transcripts, or model responses to application
logs. Each finalized turn streams Claude's reply and sends it to TTS a sentence
at a time, so the first words play while the rest is still being generated. A
turn therefore uses streaming STT, one streamed Claude call, and **one TTS call
per sentence** (a one-sentence reply is a single call); size spend limits for a
handful of TTS calls per turn. `VOICE_MAX_OUTPUT_TOKENS` caps the reply length
and `MAX_TTS_CHARS` caps the total characters spoken across those calls.

## 7. Attach a custom domain

After the first deployment, open **Workers & Pages > iot-gateway > Settings > Domains & Routes**, choose **Add > Custom Domain**, and enter a hostname such as `iot-api.example.com`.

Keep `workers.dev` enabled until the custom hostname is verified. You can then disable the public `workers.dev` route if the custom domain should be the only endpoint.

Alternatively, manage the Custom Domain in `wrangler.jsonc`:

```jsonc
"routes": [
  {
    "pattern": "iot-api.example.com",
    "custom_domain": true
  }
]
```

Use the exact hostname without `/*`. The hostname must belong to an active zone in the same Cloudflare account.

## 8. Configure cost protection

In AI Gateway, create:

1. A global spend limit for the entire gateway.
2. A spend limit split by the metadata key `device_id`.
3. Optional timeout, retry, or cheaper-model fallback rules.

The Worker sets `device_id` itself after authentication, so devices cannot claim another device's budget bucket.

## When voice stops working but text still answers

Claude runs on your Anthropic key through AI Gateway, while Flux, Smart Turn
and Aura are Cloudflare's own models and spend Workers AI neurons. An exhausted
allowance therefore breaks speech while `/v1/infer` keeps working. Workers AI
answers with HTTP 429 and:

```json
{"errors":[{"message":"AiError: ... you have used up your daily free allocation
of 10,000 neurons ...","code":4006}],"success":false}
```

The Worker turns that into a 429 `ai_budget_or_rate_limit` for the device.
Confirm it is account-wide, rather than a gateway problem, with a direct call:

```sh
curl -X POST "https://api.cloudflare.com/client/v4/accounts/ACCOUNT_ID/ai/run/@cf/deepgram/aura-1" \
  --header "Authorization: Bearer CLOUDFLARE_API_TOKEN" \
  --header "Content-Type: application/json" \
  --data '{"text":"test","speaker":"arcas"}'
```

Bunty opens the Flux socket only after its microphone hears speech, so a quiet
room costs nothing; audio recorded while the socket opens is still sent.

## Production notes

- Use one token per physical device. Never ship the same credential across a fleet.
- Tokens are bearer credentials and therefore must only be sent over HTTPS.
- A provisioning token is printed to the terminal once; do not put it in source control or firmware repositories.
- D1 provides quick revocation. For higher-assurance hardware, replace bearer authentication with signed requests or mTLS backed by a secure element.
- The Worker does not add CORS headers because IoT clients do not need browser access.
- Logs intentionally contain request IDs and device IDs, but not tokens, inputs, contexts, or model responses.
- Live PCM, transcripts, and spoken replies are not written to application logs; keep `AI_GATEWAY_LOGS` disabled if they must also stay out of Gateway logs.

## Cloudflare references

- [AI Gateway Worker bindings](https://developers.cloudflare.com/ai-gateway/usage/worker-binding-methods/)
- [AI Gateway BYOK provider keys](https://developers.cloudflare.com/ai-gateway/configuration/bring-your-own-keys/)
- [Claude Haiku model input](https://developers.cloudflare.com/ai/models/anthropic/claude-haiku-4.5/)
- [AI Gateway realtime WebSockets](https://developers.cloudflare.com/ai-gateway/usage/websockets-api/realtime-api/)
- [Deepgram Flux live STT](https://developers.cloudflare.com/workers-ai/models/flux/)
- [Aura 1 and Arcas speaker](https://developers.cloudflare.com/workers-ai/models/aura-1/)
- [D1 migrations](https://developers.cloudflare.com/d1/reference/migrations/)
- [Workers Rate Limiting binding](https://developers.cloudflare.com/workers/runtime-apis/bindings/rate-limit/)
- [Workers Custom Domains](https://developers.cloudflare.com/workers/configuration/routing/custom-domains/)

## Muse reply TTS

Muse firmware uses `POST /v1/tts` with the normal device bearer token and JSON
`{"text":"Reply text from Muse"}`. This endpoint calls only the configured Aura
TTS model, never Flux or Claude. Device authentication, rate limiting, request
body limits and `MAX_TTS_CHARS` apply. Audio streams as raw mono 16 kHz
little-endian PCM with `Content-Type: audio/l16;rate=16000;channels=1` and
`X-Audio-Byte-Order: little-endian` (the gateway's PCM convention, not RFC
network-endian L16). The existing browser and live conversation endpoints are
unchanged.
