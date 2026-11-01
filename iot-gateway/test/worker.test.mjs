import assert from "node:assert/strict";
import test from "node:test";

import worker from "../src/index.js";
import { hashDeviceToken } from "../scripts/device-lib.mjs";

const validToken = "iot_worker_test_token";
const validHash = hashDeviceToken(validToken);

function makeEnv(overrides = {}) {
  const calls = [];

  const env = {
    AI_GATEWAY_ID: "iot-test",
    AI_MODEL: "anthropic/test-model",
    STT_MODEL: "@cf/deepgram/flux",
    TTS_MODEL: "@cf/deepgram/aura-1",
    TTS_SPEAKER: "arcas",
    AI_GATEWAY_LOGS: "false",
    DEFAULT_MAX_OUTPUT_TOKENS: "64",
    GLOBAL_MAX_OUTPUT_TOKENS: "128",
    VOICE_MAX_OUTPUT_TOKENS: "48",
    MAX_REQUEST_BYTES: "2048",
    MAX_TRANSCRIPT_CHARS: "180",
    MAX_TTS_CHARS: "160",
    LIVE_AUDIO_SAMPLE_RATE: "16000",
    LIVE_MAX_SESSION_SECONDS: "300",
    LIVE_MAX_AUDIO_SECONDS: "240",
    LIVE_MAX_FRAME_BYTES: "4096",
    LIVE_MAX_TURNS: "8",
    LIVE_PLAYBACK_ACK_TIMEOUT_MS: "30000",
    STT_EOT_THRESHOLD: "0.7",
    STT_EAGER_EOT_THRESHOLD: "0.7",
    STT_EOT_TIMEOUT_MS: "1200",
    TURN_DETECTION_MODEL: "@cf/pipecat-ai/smart-turn-v2",
    TURN_DETECTION_THRESHOLD: "0.65",
    TURN_DETECTION_AUDIO_SECONDS: "12",
    TURN_DETECTION_FALLBACK_MS: "1500",
    SYSTEM_PROMPT: "Fixed trusted system prompt.",
    VOICE_SYSTEM_PROMPT: "Fixed trusted voice prompt.",
    UI_PASSWORD: "a-long-test-voice-password",
    DEVICES_DB: {
      prepare() {
        return {
          bind(hash) {
            return {
              async first() {
                if (hash !== validHash) return null;
                return {
                  id: "device-01",
                  enabled: 1,
                  max_input_chars: 200,
                  max_output_tokens: 100,
                };
              },
            };
          },
        };
      },
    },
    DEVICE_RATE_LIMITER: {
      async limit() {
        return { success: true };
      },
    },
    DEVICE_SESSION_RATE_LIMITER: {
      async limit() {
        return { success: true };
      },
    },
    AI: {
      async run(...args) {
        calls.push(args);
        return { content: [{ type: "text", text: "ok" }] };
      },
    },
    ...overrides,
  };

  return { env, calls };
}

function inferenceRequest(token, body) {
  return new Request("https://gateway.example/v1/infer", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify(body),
  });
}

function liveRequest(token, requestId = "live:42") {
  return new Request("https://gateway.example/v1/live", {
    method: "GET",
    headers: {
      Authorization: `Bearer ${token}`,
      Upgrade: "websocket",
      "X-Request-ID": requestId,
    },
  });
}

test("the Worker rejects an unknown device before calling AI", async () => {
  const { env, calls } = makeEnv();
  const response = await worker.fetch(
    inferenceRequest("iot_wrong_token", { input: "Hello" }),
    env,
  );

  assert.equal(response.status, 401);
  assert.equal(calls.length, 0);
});

test("the browser voice UI requires a password and uses a signed session", async () => {
  const { env, calls } = makeEnv();
  const page = await worker.fetch(new Request("https://gateway.example/"), env);
  assert.equal(page.status, 200);
  assert.match(await page.text(), /Access password/);

  const login = await worker.fetch(
    new Request("https://gateway.example/v1/voice/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: env.UI_PASSWORD }),
    }),
    env,
  );
  assert.equal(login.status, 204);
  const cookie = login.headers.get("set-cookie").split(";")[0];

  const chat = await worker.fetch(
    new Request("https://gateway.example/v1/voice/chat", {
      method: "POST",
      headers: { Cookie: cookie, "Content-Type": "application/json" },
      body: JSON.stringify({ input: "How is the setup?" }),
    }),
    env,
  );
  assert.equal(chat.status, 200);
  assert.equal((await chat.json()).reply, "ok");
  assert.equal(calls.length, 1);
  assert.equal(calls[0][1].system, "Fixed trusted voice prompt.");
  assert.equal(calls[0][2].gateway.metadata.device_id, "voice-ui");
});

test("the browser voice UI can enter the same live STT and TTS pipeline", async () => {
  const { env, calls } = makeEnv();
  const login = await worker.fetch(
    new Request("https://gateway.example/v1/voice/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: env.UI_PASSWORD }),
    }),
    env,
  );
  const cookie = login.headers.get("set-cookie").split(";")[0];

  const response = await worker.fetch(
    new Request("https://gateway.example/v1/voice/live", {
      headers: { Upgrade: "websocket", Cookie: cookie },
    }),
    env,
  );

  // The mock does not expose a Flux WebSocket. Reaching this point proves the
  // signed browser session was accepted and the real STT setup was invoked.
  assert.equal(response.status, 502);
  assert.equal(calls.length, 1);
  assert.equal(calls[0][0], "@cf/deepgram/flux");
  assert.equal(calls[0][2].gateway.metadata.device_id, "voice-ui");
});

test("the browser voice UI speaks typed replies with Aura", async () => {
  const { env, calls } = makeEnv();
  env.AI = {
    async run(...args) {
      calls.push(args);
      return new Response(new Uint8Array([1, 2, 3]), {
        headers: { "Content-Type": "audio/mpeg" },
      });
    },
  };
  const login = await worker.fetch(
    new Request("https://gateway.example/v1/voice/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: env.UI_PASSWORD }),
    }),
    env,
  );
  const cookie = login.headers.get("set-cookie").split(";")[0];

  const response = await worker.fetch(
    new Request("https://gateway.example/v1/voice/speak", {
      method: "POST",
      headers: { Cookie: cookie, "Content-Type": "application/json" },
      body: JSON.stringify({ text: "Hello from Bunty." }),
    }),
    env,
  );

  assert.equal(response.status, 200);
  assert.equal(response.headers.get("content-type"), "audio/mpeg");
  assert.deepEqual(Array.from(new Uint8Array(await response.arrayBuffer())), [1, 2, 3]);
  assert.equal(calls[0][0], "@cf/deepgram/aura-1");
  assert.equal(calls[0][1].speaker, "arcas");
  assert.equal(calls[0][2].returnRawResponse, true);
});

test("the Worker supplies trusted model, prompt, metadata, and caps", async () => {
  const { env, calls } = makeEnv();
  const response = await worker.fetch(
    inferenceRequest(validToken, {
      input: "Explain the sensor reading",
      context: { humidity: 77 },
      max_output_tokens: 90,
      request_id: "sensor:42",
    }),
    env,
  );

  assert.equal(response.status, 200);
  assert.equal(calls.length, 1);

  const [model, input, options] = calls[0];
  assert.equal(model, "anthropic/test-model");
  assert.equal(input.system, "Fixed trusted system prompt.");
  assert.equal(input.max_tokens, 90);
  assert.match(input.messages[0].content, /"humidity":77/);
  assert.deepEqual(options.gateway.metadata, {
    device_id: "device-01",
    request_id: "sensor:42",
  });

  const body = await response.json();
  assert.equal(body.request_id, "sensor:42");
});

test("the Worker applies rate limiting before reading the body", async () => {
  const { env, calls } = makeEnv({
    DEVICE_RATE_LIMITER: {
      async limit() {
        return { success: false };
      },
    },
  });

  const response = await worker.fetch(
    inferenceRequest(validToken, { input: "Hello" }),
    env,
  );

  assert.equal(response.status, 429);
  assert.equal(response.headers.get("retry-after"), "60");
  assert.equal(calls.length, 0);
});

test("the live route requires a WebSocket upgrade", async () => {
  const { env, calls } = makeEnv();
  const response = await worker.fetch(
    new Request("https://gateway.example/v1/live"),
    env,
  );

  assert.equal(response.status, 426);
  assert.equal(response.headers.get("upgrade"), "websocket");
  assert.equal(calls.length, 0);
});

test("the live route authenticates before starting Flux", async () => {
  const { env, calls } = makeEnv();
  const response = await worker.fetch(liveRequest("iot_wrong_token"), env);

  assert.equal(response.status, 401);
  assert.equal(calls.length, 0);
});

test("the live route rate-limits session openings before starting Flux", async () => {
  const { env, calls } = makeEnv({
    DEVICE_SESSION_RATE_LIMITER: {
      async limit() {
        return { success: false };
      },
    },
  });
  const response = await worker.fetch(liveRequest(validToken), env);

  assert.equal(response.status, 429);
  assert.equal(response.headers.get("retry-after"), "60");
  assert.equal(calls.length, 0);
});

test("the live route opens Flux with raw linear16 turn detection settings", async () => {
  const { env, calls } = makeEnv();
  const response = await worker.fetch(liveRequest(validToken), env);

  // The default AI mock intentionally does not return a WebSocket. Reaching
  // this error proves that authentication/configuration reached Flux setup.
  assert.equal(response.status, 502);
  assert.equal(calls.length, 1);

  const [model, input, options] = calls[0];
  assert.equal(model, "@cf/deepgram/flux");
  assert.deepEqual(input, {
    encoding: "linear16",
    sample_rate: "16000",
    eager_eot_threshold: "0.7",
    eot_threshold: "0.7",
    eot_timeout_ms: "1200",
  });
  assert.equal(options.websocket, true);
  assert.deepEqual(options.gateway.metadata, {
    device_id: "device-01",
    request_id: "live:42",
    pipeline_stage: "stt",
  });
});

test("physical-device route negotiates gateway turn detection only when requested", async (t) => {
  const sockets = [];
  class Socket {
    constructor() { this.readyState = 0; this.sent = []; this.listeners = {}; }
    accept() { this.readyState = 1; }
    addEventListener(type, callback) { this.listeners[type] = callback; }
    send(value) { this.sent.push(value); }
    close() {
      if (this.readyState === 3) return;
      this.readyState = 3;
      this.listeners.close?.({});
    }
  }
  const originalPair = Object.getOwnPropertyDescriptor(globalThis, "WebSocketPair");
  Object.defineProperty(globalThis, "WebSocketPair", { configurable: true, value: class {
    constructor() {
      this[0] = new Socket();
      this[1] = new Socket();
      sockets.push(this[1]);
    }
  } });
  // Node's Response rejects 101; only adapt that Workers-only constructor.
  const RealResponse = globalThis.Response;
  t.mock.method(globalThis, "Response", function (body, init) {
    return init?.status === 101 ? { status: 101, webSocket: init.webSocket }
                               : new RealResponse(body, init);
  });
  try {
    for (const requested of [false, true]) {
      const { env } = makeEnv({ AI: { async run() { return { webSocket: new Socket() }; } } });
      const request = liveRequest(validToken);
      if (requested) request.headers.set("X-Bunty-Turn-Detection", "gateway");
      const response = await worker.fetch(request, env);
      assert.equal(response.status, 101);
      const downstream = sockets.at(-1);
      const ready = JSON.parse(downstream.sent[0]);
      assert.equal(ready.turn_detection, requested ? "smart-turn-v2" : "device_vad");
      assert.equal(ready.output_audio.encoding, "mp3"); // Device audio/pins unchanged.
      downstream.close();
    }
  } finally {
    for (const socket of sockets) socket.close();
    if (originalPair) Object.defineProperty(globalThis, "WebSocketPair", originalPair);
    else delete globalThis.WebSocketPair;
  }
});

test("device TTS synthesizes Muse text without invoking STT or an LLM", async () => {
  const { env, calls } = makeEnv();
  env.AI = { async run(...args) {
    calls.push(args);
    return new Response(new Uint8Array([1, 0, 2, 0]), { headers: { "Content-Type": "audio/l16" } });
  } };
  const response = await worker.fetch(new Request("https://gateway.example/v1/tts", {
    method: "POST", headers: { Authorization: `Bearer ${validToken}`, "Content-Type": "application/json" },
    body: JSON.stringify({ text: "Reply supplied by Muse." }),
  }), env);
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("content-type"), "audio/l16;rate=16000;channels=1");
  assert.equal(response.headers.get("x-audio-byte-order"), "little-endian");
  assert.deepEqual([...new Uint8Array(await response.arrayBuffer())], [1, 0, 2, 0]);
  assert.equal(calls.length, 1);
  assert.equal(calls[0][0], "@cf/deepgram/aura-1");
  assert.equal(calls[0][1].text, "Reply supplied by Muse.");
  assert.equal(calls[0][1].encoding, "linear16");
  assert.equal(calls[0][1].sample_rate, 16000);
  assert.equal(calls[0][2].gateway.metadata.device_id, "device-01");
});

test("device TTS rejects missing credentials, oversized text, and rate limited requests", async () => {
  const { env, calls } = makeEnv();
  const request = (text, auth = true) => new Request("https://gateway.example/v1/tts", {
    method: "POST", headers: { ...(auth ? { Authorization: `Bearer ${validToken}` } : {}), "Content-Type": "application/json" },
    body: JSON.stringify({ text }),
  });
  assert.equal((await worker.fetch(request("Hello", false), env)).status, 401);
  assert.equal((await worker.fetch(request("x".repeat(161)), env)).status, 400);
  assert.equal((await worker.fetch(request(""), env)).status, 400);
  env.DEVICE_RATE_LIMITER = { async limit() { return { success: false }; } };
  assert.equal((await worker.fetch(request("Hello"), env)).status, 429);
  assert.equal(calls.length, 0);
});

test("device TTS refuses an upstream JSON response instead of labelling it as PCM", async () => {
  const { env } = makeEnv();
  env.AI = { async run() { return Response.json({ error: "upstream failed" }); } };
  const response = await worker.fetch(new Request("https://gateway.example/v1/tts", {
    method: "POST", headers: { Authorization: `Bearer ${validToken}`, "Content-Type": "application/json" },
    body: JSON.stringify({ text: "Hello." }),
  }), env);
  assert.equal(response.status, 502);
  assert.equal((await response.json()).error.code, "upstream_error");
});

/** Decode the framed speech stream back into its cues and audio, in order. */
function readSpeechFrames(bytes) {
  const frames = [];
  for (let offset = 0; offset < bytes.length;) {
    const length = bytes[offset + 1] | (bytes[offset + 2] << 8) | (bytes[offset + 3] << 16);
    frames.push({ type: bytes[offset], payload: bytes.slice(offset + 4, offset + 4 + length) });
    offset += 4 + length;
  }
  return frames;
}

function speakRequest(text) {
  return new Request("https://gateway.example/v1/speak", {
    method: "POST",
    headers: { Authorization: `Bearer ${validToken}`, "Content-Type": "application/json" },
    body: JSON.stringify({ text }),
  });
}

function pcmResponse(...bytes) {
  return new Response(new Uint8Array(bytes), { headers: { "content-type": "audio/l16" } });
}

test("device speech places each sentence's cue exactly before its audio", async () => {
  const spoken = [];
  const { env } = makeEnv({
    AI: {
      async run(model, input) {
        assert.equal(model, "@cf/deepgram/aura-1");
        spoken.push(input.text);
        return pcmResponse(spoken.length, 0, spoken.length, 0);
      },
    },
  });
  const response = await worker.fetch(
    speakRequest("[happy] [nod] [coffee] Bunty is awake. [sad] The coffee is gone."),
    env,
  );
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("content-type"), "application/vnd.bunty.speech;rate=16000;channels=1");
  assert.equal(response.headers.get("x-audio-byte-order"), "little-endian");

  const frames = readSpeechFrames(new Uint8Array(await response.arrayBuffer()));
  assert.deepEqual(spoken, ["Bunty is awake.", "The coffee is gone."]);
  assert.deepEqual(frames.map((frame) => frame.type), [4, 2, 1, 2, 1, 3]);
  assert.equal(new TextDecoder().decode(frames[0].payload.slice(0, 6)), "GIF89a");
  assert.equal(new TextDecoder().decode(frames[1].payload), "happy,nod");
  assert.deepEqual([...frames[2].payload], [1, 0, 1, 0]);
  assert.equal(new TextDecoder().decode(frames[3].payload), "sad,");
  assert.deepEqual([...frames[4].payload], [2, 0, 2, 0]);
  assert.deepEqual([...frames[5].payload], [8, 0, 0, 0]);
});

test("device speech synthesizes one sentence ahead, not the whole reply at once", async () => {
  const started = [];
  const { env } = makeEnv({
    AI: {
      async run(_model, input) {
        started.push(input.text);
        return pcmResponse(1, 0);
      },
    },
  });
  const response = await worker.fetch(
    speakRequest("[happy] First sentence here. [sad] Second sentence here. [soft] Third sentence here."),
    env,
  );
  const reader = response.body.getReader();
  await reader.read();
  assert.ok(!started.includes("Third sentence here."));
  await reader.cancel();
});

test("device speech returns the upstream status when no audio has started", async () => {
  const { env } = makeEnv({
    AI: {
      async run() {
        throw Object.assign(new Error("limit"), { status: 429 });
      },
    },
  });
  const response = await worker.fetch(speakRequest("Hello there friend."), env);
  assert.equal(response.status, 429);
  assert.equal((await response.json()).error.code, "ai_budget_or_rate_limit");
});

test("device speech keeps earlier audio when a later sentence fails", async () => {
  let calls = 0;
  const { env } = makeEnv({
    AI: {
      async run() {
        calls += 1;
        if (calls === 2) throw new Error("second sentence failed");
        return pcmResponse(7, 0);
      },
    },
  });
  const response = await worker.fetch(
    speakRequest("[neutral] First sentence here. [neutral] Second sentence here."),
    env,
  );
  assert.equal(response.status, 200);
  const frames = readSpeechFrames(new Uint8Array(await response.arrayBuffer()));
  assert.deepEqual(frames.map((frame) => frame.type), [2, 1, 3]);
  assert.deepEqual([...frames[2].payload], [2, 0, 0, 0]);
});

test("device speech refuses text with nothing to say and requires a credential", async () => {
  const { env, calls } = makeEnv();
  assert.equal((await worker.fetch(speakRequest("[happy] \u{1F600}"), env)).status, 400);
  assert.equal((await worker.fetch(new Request("https://gateway.example/v1/speak", {
    method: "POST", body: JSON.stringify({ text: "Hello" }),
  }), env)).status, 401);
  assert.equal((await worker.fetch(new Request("https://gateway.example/v1/speak"), env)).status, 405);
  assert.equal(calls.length, 0);
});
