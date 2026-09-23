import assert from "node:assert/strict";
import test from "node:test";

import {
  LiveSession,
  LiveSessionError,
  normalizeSpeechText,
  openFluxSocket,
  readLiveHandshake,
  splitEmotion,
} from "../src/live.js";

class FakeSocket {
  constructor() {
    this.readyState = 0;
    this.sent = [];
    this.listeners = new Map();
    this.closed = null;
  }

  accept() {
    this.readyState = 1;
  }

  addEventListener(type, listener) {
    const listeners = this.listeners.get(type) || [];
    listeners.push(listener);
    this.listeners.set(type, listeners);
  }

  send(value) {
    if (this.readyState !== 1) throw new Error("socket is not open");
    if (ArrayBuffer.isView(value)) {
      this.sent.push(new Uint8Array(value.buffer, value.byteOffset, value.byteLength).slice());
      return;
    }
    if (value instanceof ArrayBuffer) {
      this.sent.push(new Uint8Array(value).slice());
      return;
    }
    this.sent.push(value);
  }

  close(code, reason) {
    if (this.readyState >= 2) return;
    this.readyState = 3;
    this.closed = { code, reason };
    for (const listener of this.listeners.get("close") || []) {
      listener({ code, reason });
    }
  }
}

function makeConfig(overrides = {}) {
  return {
    gatewayId: "iot-test",
    model: "anthropic/test-haiku",
    sttModel: "@cf/deepgram/flux",
    ttsModel: "@cf/deepgram/aura-1",
    ttsSpeaker: "arcas",
    voiceSystemPrompt: "Reply in one line.",
    collectLogs: false,
    voiceMaxOutputTokens: 48,
    globalMaxOutputTokens: 128,
    maxTranscriptChars: 200,
    maxTtsChars: 160,
    liveAudioSampleRate: 16000,
    liveMaxSessionSeconds: 300,
    liveMaxAudioSeconds: 240,
    liveMaxFrameBytes: 4096,
    liveMaxTurns: 8,
    livePlaybackAckTimeoutMs: 30000,
    sttEotThreshold: 0.7,
    sttEagerEotThreshold: 0.7,
    sttEotTimeoutMs: 1200,
    turnDetectionModel: "@cf/pipecat-ai/smart-turn-v2",
    turnDetectionThreshold: 0.65,
    turnDetectionAudioSeconds: 12,
    turnDetectionFallbackMs: 1500,
    historyTurns: 6,
    ...overrides,
  };
}

function makeSession({ aiRun, rateLimit, config, useTurnDetection = true } = {}) {
  const calls = [];
  const downstream = new FakeSocket();
  const upstream = new FakeSocket();
  const env = {
    DEVICE_RATE_LIMITER: {
      async limit(...args) {
        return rateLimit ? rateLimit(...args) : { success: true };
      },
    },
    AI: {
      async run(...args) {
        calls.push(args);
        return aiRun(...args, calls.length);
      },
    },
  };
  const session = new LiveSession({
    env,
    device: {
      id: "device-01",
      maxInputChars: 180,
      maxOutputTokens: 100,
      useTurnDetection,
    },
    config: makeConfig(config),
    requestId: "live:42",
    downstream,
    upstream,
  });
  session.start();
  return { session, downstream, upstream, calls };
}

function jsonMessages(socket) {
  return socket.sent
    .filter((message) => typeof message === "string")
    .map((message) => JSON.parse(message));
}

test("readLiveHandshake accepts only a WebSocket GET with a safe request ID", () => {
  const parsed = readLiveHandshake(
    new Request("https://gateway.example/v1/live", {
      headers: {
        Upgrade: "websocket",
        "X-Request-ID": "bunty:live:1",
      },
    }),
  );
  assert.deepEqual(parsed, { requestId: "bunty:live:1", sessionId: null, useTurnDetection: false });
  const optedIn = readLiveHandshake(new Request("https://gateway.example/v1/live", {
    headers: { Upgrade: "websocket", "X-Bunty-Turn-Detection": "gateway" },
  }));
  assert.equal(optedIn.useTurnDetection, true);

  const browserParsed = readLiveHandshake(
    new Request("https://gateway.example/v1/live?session=browser123", {
      headers: { Upgrade: "websocket" },
    }),
  );
  assert.equal(browserParsed.sessionId, "browser123");

  assert.throws(
    () => readLiveHandshake(new Request("https://gateway.example/v1/live")),
    (error) =>
      error instanceof LiveSessionError && error.code === "websocket_required",
  );
});

test("openFluxSocket uses the real-time Flux WebSocket schema", async () => {
  const calls = [];
  const socket = new FakeSocket();
  const returned = await openFluxSocket(
    {
      AI: {
        async run(...args) {
          calls.push(args);
          return { webSocket: socket };
        },
      },
    },
    "device-01",
    makeConfig(),
    "live:42",
  );

  assert.equal(returned, socket);
  assert.equal(calls.length, 1);
  assert.equal(calls[0][0], "@cf/deepgram/flux");
  assert.deepEqual(calls[0][1], {
    encoding: "linear16",
    sample_rate: "16000",
    eager_eot_threshold: "0.7",
    eot_threshold: "0.7",
    eot_timeout_ms: "1200",
  });
  assert.equal(calls[0][2].websocket, true);
  assert.equal(calls[0][2].gateway.metadata.pipeline_stage, "stt");
});

test("LiveSession asks Smart Turn to confirm a Flux pause before ending input", async () => {
  const { session, downstream, upstream, calls } = makeSession({
    aiRun: async () => ({ is_complete: true, probability: 0.82 }),
  });

  await session.handleClientMessage({
    data: new Uint8Array([1, 2, 3, 4]).buffer,
  });
  session.handleUpstreamMessage({
    data: JSON.stringify({ event: "EagerEndOfTurn", turn_index: 0 }),
  });
  await new Promise((resolve) => setTimeout(resolve, 0));

  assert.equal(calls.length, 1);
  assert.equal(calls[0][0], "@cf/pipecat-ai/smart-turn-v2");
  assert.equal(calls[0][2].gateway.metadata.pipeline_stage, "turn_detection");
  const wav = new Uint8Array(await new Response(calls[0][1].audio.body).arrayBuffer());
  assert.deepEqual(Array.from(wav.slice(0, 4)), [82, 73, 70, 70]);
  assert.deepEqual(upstream.sent.slice(1), [
    JSON.stringify({ type: "ForceEndTurn" }),
    JSON.stringify({ type: "CloseStream" }),
  ]);
  assert.ok(
    jsonMessages(downstream).some(
      (message) =>
        message.type === "turn_detection" && message.complete === true,
    ),
  );
  session.stop(true, 1000, "test complete");
});

test("gateway-owned EndOfTurn without eager event still invokes turn detection", async () => {
  const { session, downstream, upstream, calls } = makeSession({
    aiRun: async () => ({ is_complete: true, probability: 0.9 }),
  });
  assert.equal(jsonMessages(downstream)[0].turn_detection, "smart-turn-v2");
  await session.handleClientMessage({ data: new Uint8Array(640).buffer });
  session.handleUpstreamMessage({ data: JSON.stringify({ event: "EndOfTurn", turn_index: 0, transcript: "hello" }) });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(calls.length, 1);
  assert.ok(jsonMessages(downstream).some(message => message.type === "input.ended"));
  assert.ok(upstream.sent.includes(JSON.stringify({ type: "ForceEndTurn" })));
  session.stop(true, 1000, "test complete");
});

test("legacy device-owned EndOfTurn still waits for input.end", async () => {
  const { session, downstream, calls } = makeSession({
    useTurnDetection: false,
    aiRun: async () => { throw new Error("no Smart Turn for legacy clients"); },
  });
  assert.equal(jsonMessages(downstream)[0].turn_detection, "device_vad");
  await session.handleClientMessage({ data: new Uint8Array(640).buffer });
  session.handleUpstreamMessage({ data: JSON.stringify({ event: "EndOfTurn", turn_index: 0, transcript: "hello" }) });
  assert.equal(calls.length, 0);
  assert.equal(session.inputEnded, false);
  session.stop(true, 1000, "test complete");
});

test("resumed speech invalidates a pending complete Smart Turn result", async () => {
  let finish;
  const { session, downstream } = makeSession({
    aiRun: () => new Promise(resolve => { finish = resolve; }),
  });
  await session.handleClientMessage({ data: new Uint8Array(640).buffer });
  session.handleUpstreamMessage({ data: JSON.stringify({ event: "EagerEndOfTurn", turn_index: 0 }) });
  session.handleUpstreamMessage({ data: JSON.stringify({ event: "TurnResumed", turn_index: 0 }) });
  finish({ is_complete: true, probability: 0.99 });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(session.inputEnded, false);
  assert.ok(!jsonMessages(downstream).some(message => message.type === "input.ended"));
  session.stop(true, 1000, "test complete");
});

test("device session.close cancels junk input without triggering a reply", async () => {
  const { session, upstream, calls } = makeSession({
    aiRun: async () => { throw new Error("closing noise must not request an AI reply"); },
  });
  await session.handleClientMessage({ data: new Uint8Array(640).buffer });
  await session.handleClientMessage({ data: JSON.stringify({ type: "session.close" }) });
  assert.equal(session.stopped, true);
  assert.equal(upstream.readyState, 3);
  assert.equal(calls.length, 0);
});

test("LiveSession forwards bounded raw PCM and partial transcripts", () => {
  const { session, downstream, upstream } = makeSession({
    aiRun: async () => {
      throw new Error("AI should not run for a partial transcript");
    },
  });

  const pcm = new Uint8Array([1, 2, 3, 4]);
  session.handleClientMessage({ data: pcm.buffer });
  assert.deepEqual(upstream.sent, [pcm]);

  session.handleUpstreamMessage({
    data: JSON.stringify({
      event: "Update",
      turn_index: 0,
      transcript: "hello Bunty",
    }),
  });
  assert.ok(
    jsonMessages(downstream).some(
      (message) =>
        message.type === "transcript.partial" && message.text === "hello Bunty",
    ),
  );

  session.stop(true, 1000, "test complete");
});

test("LiveSession preserves Blob audio ordering and lets device VAD end input", async () => {
  const { session, downstream, upstream } = makeSession({
    aiRun: async () => {
      throw new Error("AI should not run before a final transcript");
    },
  });

  await session.handleClientMessage({
    data: new Blob([new Uint8Array([1, 2, 3, 4])]),
  });
  await session.handleClientMessage({
    data: JSON.stringify({ type: "input.end" }),
  });

  assert.deepEqual(upstream.sent, [
    new Uint8Array([1, 2, 3, 4]),
    JSON.stringify({ type: "ForceEndTurn" }),
    JSON.stringify({ type: "CloseStream" }),
  ]);
  assert.ok(
    jsonMessages(downstream).some((message) => message.type === "input.ended"),
  );
  session.stop(true, 1000, "test complete");
});

test("LiveSession preserves the last Update when Flux closes during finalization", async () => {
  const { session, downstream, upstream, calls } = makeSession({
    aiRun: async (_model, _input, _options, callNumber) => {
      if (callNumber === 1) {
        return { content: [{ type: "text", text: "The voice path works." }] };
      }
      return new Response(new Uint8Array([10, 20, 30, 40]), {
        headers: { "Content-Type": "audio/l16" },
      });
    },
  });

  session.handleUpstreamMessage({
    data: JSON.stringify({
      event: "Update",
      turn_index: 0,
      transcript: "Test",
    }),
  });
  session.handleUpstreamMessage({
    data: JSON.stringify({
      event: "EndOfTurn",
      turn_index: 0,
      transcript: "Test",
    }),
  });
  assert.equal(calls.length, 0, "Flux cannot outrun the device VAD");

  session.handleUpstreamMessage({
    data: JSON.stringify({
      event: "Update",
      turn_index: 1,
      transcript: "the voice path",
    }),
  });
  await session.handleClientMessage({
    data: new Uint8Array([1, 2, 3, 4]),
  });
  await session.handleClientMessage({
    data: JSON.stringify({ type: "input.end" }),
  });

  upstream.close(1000, "Flux stream drained");
  await session.whenIdle();

  assert.equal(calls.length, 2);
  assert.equal(calls[0][1].messages[0].content, "Test the voice path");
  const controls = jsonMessages(downstream);
  assert.ok(
    controls.some(
      (message) =>
        message.type === "transcript.final" &&
        message.text === "Test the voice path",
    ),
  );
  assert.ok(!controls.some((message) => message.code === "no_speech"));

  session.handleClientMessage({
    data: JSON.stringify({ type: "playback.finished", turn: "0" }),
  });
});

test("LiveSession runs final speech through Haiku and streams Aura Arcas PCM", async () => {
  const { session, downstream, calls } = makeSession({
    aiRun: async (_model, _input, _options, callNumber) => {
      if (callNumber === 1) {
        return {
          content: [{ type: "text", text: "  It is\n twenty-four degrees.  " }],
        };
      }
      return new Response(new Uint8Array([10, 20, 30, 40]), {
        headers: { "Content-Type": "audio/l16" },
      });
    },
  });

  await session.handleClientMessage({
    data: new Uint8Array([1, 2, 3, 4]),
  });
  await session.handleClientMessage({
    data: JSON.stringify({ type: "input.end" }),
  });
  session.handleUpstreamMessage({
    data: JSON.stringify({
      event: "EndOfTurn",
      turn_index: 3,
      transcript: "What is the temperature?",
    }),
  });
  await session.whenIdle();

  assert.equal(calls.length, 2);
  const [model, modelInput, modelOptions] = calls[0];
  assert.equal(model, "anthropic/test-haiku");
  assert.equal(modelInput.system, "Reply in one line.");
  assert.equal(modelInput.max_tokens, 48);
  assert.equal(modelInput.messages[0].content, "What is the temperature?");
  assert.deepEqual(modelOptions.gateway.metadata, {
    device_id: "device-01",
    request_id: "live:42",
    pipeline_stage: "llm",
    turn: "3",
  });

  const [ttsModel, ttsInput, ttsOptions] = calls[1];
  assert.equal(ttsModel, "@cf/deepgram/aura-1");
  assert.deepEqual(ttsInput, {
    text: "It is twenty-four degrees.",
    speaker: "arcas",
    encoding: "linear16",
    container: "none",
    sample_rate: 16000,
  });
  assert.equal(ttsOptions.returnRawResponse, true);
  assert.equal(ttsOptions.gateway.metadata.pipeline_stage, "tts");

  const controls = jsonMessages(downstream);
  const controlTypes = controls.map((message) => message.type);
  assert.ok(controlTypes.includes("transcript.final"));
  assert.ok(controlTypes.includes("input.pause"));
  assert.ok(controlTypes.includes("assistant.thinking"));
  assert.ok(
    controls.some(
      (message) =>
        message.type === "assistant.text" &&
        message.text === "It is twenty-four degrees.",
    ),
  );
  assert.ok(controlTypes.includes("audio.start"));
  assert.ok(controlTypes.includes("audio.end"));
  assert.ok(!controlTypes.includes("input.resume"));
  assert.deepEqual(
    downstream.sent.filter((message) => message instanceof Uint8Array),
    [new Uint8Array([10, 20, 30, 40])],
  );

  session.handleClientMessage({
    data: JSON.stringify({ type: "playback.finished", turn: "3" }),
  });
  assert.ok(
    jsonMessages(downstream).some(
      (message) => message.type === "session.complete" && message.turn === "3",
    ),
  );
  assert.deepEqual(downstream.closed, {
    code: 1000,
    reason: "Voice turn complete",
  });

  session.stop(true, 1000, "test complete");
});

function sseStream(parts) {
  return new ReadableStream({
    start(controller) {
      const encoder = new TextEncoder();
      for (const part of parts) controller.enqueue(encoder.encode(part));
      controller.close();
    },
  });
}

test("LiveSession speaks a streamed reply sentence by sentence", async () => {
  const ttsTexts = [];
  const { session, downstream, calls } = makeSession({
    aiRun: async (model, input) => {
      if (model === "anthropic/test-haiku") {
        return sseStream([
          'data: {"response":"Hello there. "}\n\n',
          'data: {"response":"How are you today? "}\n\n',
          'data: {"response":"Bye."}\n\n',
          "data: [DONE]\n\n",
        ]);
      }
      ttsTexts.push(input.text);
      return new Response(new Uint8Array([1, 2]), {
        headers: { "Content-Type": "audio/l16" },
      });
    },
  });

  await session.handleClientMessage({ data: new Uint8Array([1, 2, 3, 4]) });
  await session.handleClientMessage({ data: JSON.stringify({ type: "input.end" }) });
  session.handleUpstreamMessage({
    data: JSON.stringify({ event: "EndOfTurn", turn_index: 0, transcript: "hi" }),
  });
  await session.whenIdle();

  assert.equal(calls[0][1].stream, true, "Claude is called in streaming mode");
  assert.deepEqual(ttsTexts, [
    "Hello there.",
    "How are you today?",
    "Bye.",
  ]);

  const controlTypes = jsonMessages(downstream).map((message) => message.type);
  assert.equal(
    controlTypes.filter((type) => type === "audio.start").length,
    1,
    "one shared audio stream for the whole reply",
  );
  assert.equal(
    controlTypes.filter((type) => type === "audio.end").length,
    1,
  );

  const finalText = jsonMessages(downstream)
    .filter((message) => message.type === "assistant.text")
    .at(-1).text;
  assert.equal(finalText, "Hello there. How are you today? Bye.");

  session.handleClientMessage({
    data: JSON.stringify({ type: "playback.finished", turn: "0" }),
  });
});

test("LiveSession falls back to a non-streaming call when the stream yields no text", async () => {
  const { session, downstream, calls } = makeSession({
    aiRun: async (model) => {
      if (model === "anthropic/test-haiku") {
        // callNumber 1: an unrecognized stream shape. callNumber 2: fallback.
        return calls.length === 1
          ? sseStream(['data: {"unknown":"shape"}\n\n', "data: [DONE]\n\n"])
          : { content: [{ type: "text", text: "Recovered reply." }] };
      }
      return new Response(new Uint8Array([9]), {
        headers: { "Content-Type": "audio/l16" },
      });
    },
  });

  await session.handleClientMessage({ data: new Uint8Array([1, 2, 3, 4]) });
  await session.handleClientMessage({ data: JSON.stringify({ type: "input.end" }) });
  session.handleUpstreamMessage({
    data: JSON.stringify({ event: "EndOfTurn", turn_index: 0, transcript: "hi" }),
  });
  await session.whenIdle();

  assert.equal(calls.length, 3, "stream attempt, non-streaming fallback, then TTS");
  const finalText = jsonMessages(downstream)
    .filter((message) => message.type === "assistant.text")
    .at(-1).text;
  assert.equal(finalText, "Recovered reply.");

  session.handleClientMessage({
    data: JSON.stringify({ type: "playback.finished", turn: "0" }),
  });
});

test("LiveSession enforces per-turn rate limits without calling AI", async () => {
  const { session, downstream, calls } = makeSession({
    rateLimit: async () => ({ success: false }),
    aiRun: async () => {
      throw new Error("AI should not run when rate limited");
    },
  });

  await session.handleClientMessage({
    data: new Uint8Array([1, 2, 3, 4]),
  });
  await session.handleClientMessage({
    data: JSON.stringify({ type: "input.end" }),
  });
  session.handleUpstreamMessage({
    data: JSON.stringify({
      event: "EndOfTurn",
      turn_index: 0,
      transcript: "Hello",
    }),
  });
  await session.whenIdle();

  assert.equal(calls.length, 0);
  const controls = jsonMessages(downstream);
  assert.ok(
    controls.some(
      (message) => message.type === "error" && message.code === "rate_limited",
    ),
  );
  assert.ok(controls.some((message) => message.type === "session.complete"));
  session.stop(true, 1000, "test complete");
});

test("LiveSession rejects oversized frames before forwarding them", () => {
  const { session, downstream, upstream } = makeSession({
    config: { liveMaxFrameBytes: 4 },
    aiRun: async () => {
      throw new Error("AI should not run");
    },
  });
  session.handleClientMessage({ data: new Uint8Array(6).buffer });

  assert.equal(upstream.sent.length, 0);
  assert.ok(
    jsonMessages(downstream).some(
      (message) =>
        message.type === "error" && message.code === "audio_frame_too_large",
    ),
  );
  session.stop(true, 1000, "test complete");
});

test("normalizeSpeechText produces a bounded single-line TTS string", () => {
  assert.equal(
    normalizeSpeechText("  first line\nsecond   line ", 80),
    "first line second line",
  );
  assert.equal(normalizeSpeechText("one two three four", 14), "one two three…");
});

test("splitEmotion strips tags and keeps the first known emotion", () => {
  assert.deepEqual(splitEmotion("[happy] Hello there."), {
    emotion: "happy",
    text: "Hello there.",
  });
  assert.deepEqual(splitEmotion("[ Angry ]No. [sad] Fine."), {
    emotion: "angry",
    text: "No. Fine.",
  });
  assert.deepEqual(splitEmotion("[grumpy] Unknown tags vanish."), {
    emotion: null,
    text: "Unknown tags vanish.",
  });
  assert.deepEqual(splitEmotion("No tags at all."), {
    emotion: null,
    text: "No tags at all.",
  });
});
