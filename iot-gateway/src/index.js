import { authenticateDevice } from "./auth.js";
import {
  clearVoiceUiSession,
  createVoiceUiSession,
  hasVoiceUiSession,
  verifyVoiceUiPassword,
} from "./ui-auth.js";
import {
  appendTurn,
  loadHistory,
  normalizeSessionId,
} from "./history.js";
import {
  LiveSessionError,
  openLiveSession,
  readLiveHandshake,
  splitEmotion,
} from "./live.js";
import { readInferenceRequest, RequestValidationError } from "./request.js";
import { speechSegments, speechStream } from "./speech.js";
import { voiceLoginPage, voiceUiPage } from "./voice-ui.js";

const INFERENCE_ROUTE = "/v1/infer";
const LIVE_ROUTE = "/v1/live";
const VOICE_LOGIN_ROUTE = "/v1/voice/login";
const VOICE_LOGOUT_ROUTE = "/v1/voice/logout";
const VOICE_CHAT_ROUTE = "/v1/voice/chat";
const VOICE_SPEAK_ROUTE = "/v1/voice/speak";
const VOICE_LIVE_ROUTE = "/v1/voice/live";
const VOICE_TURN_ROUTE = "/v1/voice/turn";
const TURN_ROUTE = "/v1/turn";

export default {
  /**
   * @param {Request} request
   * @param {Record<string, any>} env
   * @returns {Promise<Response>}
   */
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method === "GET" && url.pathname === "/health") {
      return jsonResponse({
        ok: true,
        service: "iot-gateway",
        endpoints: [INFERENCE_ROUTE, LIVE_ROUTE],
      });
    }

    if (url.pathname === "/") {
      if (request.method !== "GET") {
        return errorResponse("method_not_allowed", "Use GET for this endpoint.", 405, {
          Allow: "GET",
        });
      }
      return handleVoiceUiPage(request, env);
    }

    if (url.pathname === VOICE_LOGIN_ROUTE) {
      return handleVoiceUiLogin(request, env);
    }

    if (url.pathname === VOICE_LOGOUT_ROUTE) {
      if (request.method !== "POST") {
        return errorResponse("method_not_allowed", "Use POST for this endpoint.", 405, {
          Allow: "POST",
        });
      }
      return new Response(null, {
        status: 204,
        headers: { "Set-Cookie": clearVoiceUiSession(request), "Cache-Control": "no-store" },
      });
    }

    if (url.pathname === VOICE_CHAT_ROUTE) {
      return handleVoiceUiChat(request, env);
    }

    if (url.pathname === "/v1/tts") {
      return handleDeviceSpeak(request, env);
    }

    if (url.pathname === "/v1/speak") {
      return handleDeviceSpeech(request, env);
    }

    if (url.pathname === VOICE_TURN_ROUTE || url.pathname === TURN_ROUTE) {
      return handleDeviceTurn(request, env);
    }

    if (url.pathname === VOICE_SPEAK_ROUTE) {
      return handleVoiceUiSpeak(request, env);
    }

    if (url.pathname === VOICE_LIVE_ROUTE) {
      return handleVoiceUiLive(request, env);
    }

    if (url.pathname === LIVE_ROUTE) {
      return handleLiveUpgrade(request, env);
    }

    if (url.pathname !== INFERENCE_ROUTE) {
      return errorResponse("not_found", "Not found.", 404);
    }

    if (request.method !== "POST") {
      return errorResponse("method_not_allowed", "Use POST for this endpoint.", 405, {
        Allow: "POST",
      });
    }

    const authentication = await authenticateRequest(request, env);
    if (authentication.response) return authentication.response;
    const device = authentication.device;

    const rateLimitResponse = await enforceRateLimit(
      env.DEVICE_RATE_LIMITER,
      device.id,
      "request",
    );
    if (rateLimitResponse) return rateLimitResponse;

    let config;
    try {
      config = readConfig(env);
    } catch (error) {
      logFailure("worker_configuration_invalid", error);
      return errorResponse("service_unavailable", "Gateway configuration is invalid.", 503);
    }

    return handleInferenceRequest(request, env, device, config);
  },
};

/** @param {Request} request @param {Record<string, any>} env */
async function handleVoiceUiPage(request, env) {
  try {
    return (await hasVoiceUiSession(request, env)) ? voiceUiPage() : voiceLoginPage();
  } catch (error) {
    logFailure("voice_ui_configuration_invalid", error);
    return errorResponse("service_unavailable", "Voice UI is not configured.", 503);
  }
}

/** @param {Request} request @param {Record<string, any>} env */
async function handleVoiceUiLogin(request, env) {
  if (request.method !== "POST") {
    return errorResponse("method_not_allowed", "Use POST for this endpoint.", 405, {
      Allow: "POST",
    });
  }

  let body;
  try {
    body = await readSmallJson(request, 1024);
  } catch {
    return errorResponse("invalid_request", "Password is required.", 400);
  }

  if (typeof body.password !== "string") {
    return errorResponse("invalid_request", "Password is required.", 400);
  }

  try {
    if (!(await verifyVoiceUiPassword(body.password, env))) {
      return errorResponse("unauthorized", "Invalid password.", 401);
    }
    return new Response(null, {
      status: 204,
      headers: {
        "Set-Cookie": await createVoiceUiSession(request, env),
        "Cache-Control": "no-store",
      },
    });
  } catch (error) {
    logFailure("voice_ui_configuration_invalid", error);
    return errorResponse("service_unavailable", "Voice UI is not configured.", 503);
  }
}

/** @param {Request} request @param {Record<string, any>} env */
async function handleVoiceUiChat(request, env) {
  if (request.method !== "POST") {
    return errorResponse("method_not_allowed", "Use POST for this endpoint.", 405, {
      Allow: "POST",
    });
  }

  try {
    if (!(await hasVoiceUiSession(request, env))) {
      return errorResponse("unauthorized", "Sign in to use voice.", 401);
    }
  } catch (error) {
    logFailure("voice_ui_configuration_invalid", error);
    return errorResponse("service_unavailable", "Voice UI is not configured.", 503);
  }

  const rateLimitResponse = await enforceRateLimit(
    env.DEVICE_RATE_LIMITER,
    "voice-ui",
    "voice_ui_request",
  );
  if (rateLimitResponse) return rateLimitResponse;

  let config;
  try {
    config = readConfig(env);
  } catch (error) {
    logFailure("worker_configuration_invalid", error);
    return errorResponse("service_unavailable", "Gateway configuration is invalid.", 503);
  }

  let body;
  try {
    body = await readSmallJson(request, config.maxRequestBytes);
  } catch {
    return errorResponse("invalid_request", "Could not read the message.", 400);
  }

  const input = typeof body.input === "string" ? body.input.trim() : "";
  if (!input || input.length > config.maxTranscriptChars) {
    return errorResponse(
      "invalid_request",
      `Message must be between 1 and ${config.maxTranscriptChars} characters.`,
      400,
    );
  }

  const requestId = `voice-ui:${crypto.randomUUID()}`;
  try {
    const result = await env.AI.run(
      config.model,
      {
        system: config.voiceSystemPrompt,
        max_tokens: Math.min(config.voiceMaxOutputTokens, config.globalMaxOutputTokens),
        messages: [{ role: "user", content: input }],
      },
      aiOptions(config, "voice-ui", requestId),
    );
    const reply = splitEmotion(assistantText(result)).text;
    if (!reply) throw new Error("AI response contained no text");
    return jsonResponse({ request_id: requestId, reply });
  } catch (error) {
    const upstreamStatus = Number(error?.status);
    const rateLimited = upstreamStatus === 429;
    logFailure("voice_ui_inference_failed", error, {
      requestId,
      upstreamStatus: Number.isFinite(upstreamStatus) ? upstreamStatus : undefined,
    });
    return errorResponse(
      rateLimited ? "ai_budget_or_rate_limit" : "upstream_error",
      rateLimited ? "AI usage limit reached. Try again later." : "The AI provider could not complete the request.",
      rateLimited ? 429 : 502,
      rateLimited ? { "Retry-After": "60" } : undefined,
    );
  }
}

/** TTS-only device route: Muse supplies the reply; no STT or LLM is called. */
async function handleDeviceSpeak(request, env) {
  if (request.method !== "POST") {
    return errorResponse("method_not_allowed", "Use POST for this endpoint.", 405, { Allow: "POST" });
  }
  const authentication = await authenticateRequest(request, env);
  if (authentication.response) return authentication.response;
  return handleVoiceUiSpeak(request, env, authentication.device);
}

/**
 * Device speech with its performance: tagged reply text in, PCM framed with
 * expression cues out. See speech.js. Like /v1/tts, this calls no STT or LLM.
 * @param {Request} request
 * @param {Record<string, any>} env
 */
async function handleDeviceSpeech(request, env) {
  if (request.method !== "POST") {
    return errorResponse("method_not_allowed", "Use POST for this endpoint.", 405, { Allow: "POST" });
  }
  const authentication = await authenticateRequest(request, env);
  if (authentication.response) return authentication.response;
  const device = authentication.device;

  const speech = await readSpeechRequest(request, env, device.id);
  if (speech.response) return speech.response;
  const { config, text } = speech;

  const segments = speechSegments(text);
  if (segments.length === 0) {
    return errorResponse("invalid_request", "There is nothing to say in that text.", 400);
  }

  const requestId = `${device.id}:speak:${crypto.randomUUID()}`;
  const audioBody = async (segment) => {
    const response = audioResponse(await synthesize(env, config, segment, device.id, requestId));
    if ((response.headers.get("Content-Type") || "").includes("json")) {
      throw new TypeError("TTS returned JSON instead of audio.");
    }
    return response.body;
  };

  let firstBody;
  try {
    firstBody = await audioBody(segments[0].text);
  } catch (error) {
    return speechFailure(error, requestId);
  }
  const body = speechStream(segments, firstBody, audioBody, (error) => {
    logFailure("device_speech_interrupted", error, { requestId });
  });
  return new Response(body, {
    headers: {
      "Content-Type": `application/vnd.bunty.speech;rate=${config.liveAudioSampleRate};channels=1`,
      "X-Audio-Byte-Order": "little-endian",
      "Cache-Control": "no-store",
      "X-Content-Type-Options": "nosniff",
    },
  });
}

/**
 * Wraps raw mono 16-bit linear PCM in a standard 44-byte WAV header.
 * @param {Uint8Array} pcm
 * @param {number} sampleRate
 * @returns {Uint8Array}
 */
function pcmToWav(pcm, sampleRate = 16000) {
  const wav = new Uint8Array(44 + pcm.byteLength);
  const view = new DataView(wav.buffer);
  wav[0] = 0x52; wav[1] = 0x49; wav[2] = 0x46; wav[3] = 0x46; // "RIFF"
  view.setUint32(4, 36 + pcm.byteLength, true);
  wav[8] = 0x57; wav[9] = 0x41; wav[10] = 0x56; wav[11] = 0x45; // "WAVE"
  wav[12] = 0x66; wav[13] = 0x6d; wav[14] = 0x74; wav[15] = 0x20; // "fmt "
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true); // PCM format
  view.setUint16(22, 1, true); // Mono = 1 channel
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true); // Byte rate
  view.setUint16(32, 2, true); // Block align
  view.setUint16(34, 16, true); // 16 bits per sample
  wav[36] = 0x64; wav[37] = 0x61; wav[38] = 0x74; wav[39] = 0x61; // "data"
  view.setUint32(40, pcm.byteLength, true);
  wav.set(pcm, 44);
  return wav;
}

/**
 * Unified turn route for physical device fallback when Muse is offline.
 * Audio in (16 kHz linear PCM or WAV) -> Whisper STT -> Claude Haiku -> Aura TTS,
 * streaming back framed PCM with expression cues (identical format to /v1/speak).
 * @param {Request} request
 * @param {Record<string, any>} env
 */
async function handleDeviceTurn(request, env) {
  if (request.method !== "POST") {
    return errorResponse("method_not_allowed", "Use POST for this endpoint.", 405, { Allow: "POST" });
  }

  const authentication = await authenticateRequest(request, env);
  if (authentication.response) return authentication.response;
  const device = authentication.device;

  const rateLimitResponse = await enforceRateLimit(env.DEVICE_RATE_LIMITER, device.id, "turn");
  if (rateLimitResponse) return rateLimitResponse;

  let config;
  try {
    config = readConfig(env);
  } catch (error) {
    logFailure("worker_configuration_invalid", error);
    return errorResponse("service_unavailable", "Gateway configuration is invalid.", 503);
  }

  const requestId = `${device.id}:turn:${crypto.randomUUID()}`;
  const maxAudioBytes = config.liveAudioSampleRate * 2 * 45; // 45 seconds max
  let audioBuffer;
  try {
    const contentLength = Number(request.headers.get("content-length"));
    if (Number.isFinite(contentLength) && contentLength > maxAudioBytes) {
      return errorResponse("payload_too_large", "Audio recording exceeds maximum duration.", 413);
    }
    audioBuffer = await request.arrayBuffer();
  } catch (error) {
    logFailure("device_turn_read_failed", error, { requestId });
    return errorResponse("invalid_request", "Could not read audio stream.", 400);
  }

  if (!audioBuffer || audioBuffer.byteLength < 1600) {
    return errorResponse("invalid_request", "Audio recording is too short.", 400);
  }

  const uint8 = new Uint8Array(audioBuffer);
  const isWav = uint8.byteLength >= 12 &&
    uint8[0] === 0x52 && uint8[1] === 0x49 && uint8[2] === 0x46 && uint8[3] === 0x46;
  const wavBytes = isWav ? uint8 : pcmToWav(uint8, config.liveAudioSampleRate);

  let sttResult;
  try {
    sttResult = await env.AI.run(
      config.batchSttModel,
      { audio: Array.from(wavBytes) },
      aiOptions(config, device.id, requestId),
    );
  } catch (error) {
    const upstreamStatus = Number(error?.status);
    const rateLimited = upstreamStatus === 429;
    logFailure("device_turn_stt_failed", error, {
      requestId,
      upstreamStatus: Number.isFinite(upstreamStatus) ? upstreamStatus : undefined,
    });
    return errorResponse(
      rateLimited ? "ai_budget_or_rate_limit" : "upstream_error",
      rateLimited ? "AI usage limit reached. Try again later." : "Speech transcription failed.",
      rateLimited ? 429 : 502,
      rateLimited ? { "Retry-After": "60" } : undefined,
    );
  }

  const userText = typeof sttResult?.text === "string" ? sttResult.text.trim() : "";
  if (!userText) {
    return errorResponse("no_speech_detected", "No speech could be recognized.", 400);
  }

  const url = new URL(request.url);
  const sessionId =
    normalizeSessionId(request.headers.get("x-bunty-session")) ||
    normalizeSessionId(url.searchParams.get("session")) ||
    null;

  let history = [];
  if (device.preserveSessionHistory !== false && sessionId && env.DEVICES_DB) {
    try {
      history = await loadHistory(env, device.id, sessionId, config.historyTurns || 6);
    } catch (error) {
      logFailure("device_turn_history_failed", error, { requestId });
    }
  }

  const maxTokens = Math.min(
    device.maxOutputTokens ?? config.voiceMaxOutputTokens,
    config.globalMaxOutputTokens,
  );
  let modelResult;
  try {
    modelResult = await env.AI.run(
      config.model,
      {
        system: config.voiceSystemPrompt,
        max_tokens: maxTokens,
        messages: [
          ...history,
          { role: "user", content: userText },
        ],
      },
      aiOptions(config, device.id, requestId),
    );
  } catch (error) {
    const upstreamStatus = Number(error?.status);
    const rateLimited = upstreamStatus === 429;
    logFailure("device_turn_inference_failed", error, {
      requestId,
      upstreamStatus: Number.isFinite(upstreamStatus) ? upstreamStatus : undefined,
    });
    return errorResponse(
      rateLimited ? "ai_budget_or_rate_limit" : "upstream_error",
      rateLimited ? "AI usage limit reached. Try again later." : "AI could not generate a reply.",
      rateLimited ? 429 : 502,
      rateLimited ? { "Retry-After": "60" } : undefined,
    );
  }

  const assistantReply = assistantText(modelResult);
  if (!assistantReply) {
    return errorResponse("upstream_error", "AI produced an empty reply.", 502);
  }

  if (device.preserveSessionHistory !== false && sessionId && env.DEVICES_DB) {
    try {
      await appendTurn(env, device.id, sessionId, userText, assistantReply, config.historyTurns || 6);
    } catch (error) {
      logFailure("device_turn_append_history_failed", error, { requestId });
    }
  }

  const segments = speechSegments(assistantReply);
  if (segments.length === 0) {
    return errorResponse("invalid_request", "There is nothing to say in that reply.", 400);
  }

  const acceptHeader = (request.headers.get("accept") || "").toLowerCase();
  const silentMode =
    acceptHeader.includes("application/json") ||
    request.headers.get("x-bunty-output") === "silent";

  if (silentMode) {
    return jsonResponse(
      {
        ok: true,
        transcript: userText,
        reply: assistantReply,
        emotion: segments[0]?.emotion || "neutral",
        gesture: segments[0]?.gesture || null,
        picture: segments[0]?.picture || null,
      },
      200,
      {
        "X-Transcript": encodeURIComponent(userText),
        "X-Reply": encodeURIComponent(assistantReply),
        "X-Emotion": segments[0]?.emotion || "neutral",
        "X-Picture": segments[0]?.picture || "",
      },
    );
  }

  const audioBody = async (segment) => {
    const response = audioResponse(await synthesize(env, config, segment, device.id, requestId));
    if ((response.headers.get("Content-Type") || "").includes("json")) {
      throw new TypeError("TTS returned JSON instead of audio.");
    }
    return response.body;
  };

  let firstBody;
  try {
    firstBody = await audioBody(segments[0].text);
  } catch (error) {
    return speechFailure(error, requestId);
  }

  const body = speechStream(segments, firstBody, audioBody, (error) => {
    logFailure("device_speech_interrupted", error, { requestId });
  });

  return new Response(body, {
    headers: {
      "Content-Type": `application/vnd.bunty.speech;rate=${config.liveAudioSampleRate};channels=1`,
      "X-Audio-Byte-Order": "little-endian",
      "Cache-Control": "no-store",
      "X-Content-Type-Options": "nosniff",
      "X-Transcript": encodeURIComponent(userText),
      "X-Reply": encodeURIComponent(assistantReply),
    },
  });
}

/**
 * Rate limit, then read and bound the requested speech text.
 * @param {Request} request
 * @param {Record<string, any>} env
 * @param {string} limitKey
 */
async function readSpeechRequest(request, env, limitKey) {
  const rateLimitResponse = await enforceRateLimit(env.DEVICE_RATE_LIMITER, limitKey, "tts");
  if (rateLimitResponse) return { response: rateLimitResponse };

  let config;
  try {
    config = readConfig(env);
  } catch (error) {
    logFailure("worker_configuration_invalid", error);
    return { response: errorResponse("service_unavailable", "Gateway configuration is invalid.", 503) };
  }

  let body;
  try {
    body = await readSmallJson(request, config.maxRequestBytes);
  } catch {
    return { response: errorResponse("invalid_request", "Could not read the speech request.", 400) };
  }
  const text = typeof body.text === "string" ? body.text.trim() : "";
  if (!text || text.length > config.maxTtsChars) {
    return {
      response: errorResponse(
        "invalid_request",
        `Speech must be between 1 and ${config.maxTtsChars} characters.`,
        400,
      ),
    };
  }
  return { config, text };
}

/**
 * @param {Record<string, any>} env
 * @param {ReturnType<typeof readConfig>} config
 * @param {string} text
 * @param {string} actorId
 * @param {string} requestId
 */
function synthesize(env, config, text, actorId, requestId) {
  return env.AI.run(
    config.ttsModel,
    {
      text,
      speaker: config.ttsSpeaker,
      encoding: "linear16",
      container: "none",
      sample_rate: config.liveAudioSampleRate,
    },
    { returnRawResponse: true, ...aiOptions(config, actorId, requestId) },
  );
}

/** @param {unknown} error @param {string} requestId */
function speechFailure(error, requestId) {
  const upstreamStatus = Number(error?.status);
  const rateLimited = upstreamStatus === 429;
  logFailure("voice_ui_tts_failed", error, {
    requestId,
    upstreamStatus: Number.isFinite(upstreamStatus) ? upstreamStatus : undefined,
  });
  return errorResponse(
    rateLimited ? "ai_budget_or_rate_limit" : "upstream_error",
    rateLimited ? "AI usage limit reached. Try again later." : "Bunty could not speak that reply.",
    rateLimited ? 429 : 502,
    rateLimited ? { "Retry-After": "60" } : undefined,
  );
}

/**
 * Synthesizes a browser UI reply with the same trusted Aura voice used by the
 * live Bunty pipeline. This keeps browser fallback audio out of platform TTS.
 * @param {Request} request
 * @param {Record<string, any>} env
 */
async function handleVoiceUiSpeak(request, env, device = null) {
  if (request.method !== "POST") {
    return errorResponse("method_not_allowed", "Use POST for this endpoint.", 405, {
      Allow: "POST",
    });
  }

  if (!device) {
    try {
      if (!(await hasVoiceUiSession(request, env))) {
        return errorResponse("unauthorized", "Sign in to use voice.", 401);
      }
    } catch (error) {
      logFailure("voice_ui_configuration_invalid", error);
      return errorResponse("service_unavailable", "Voice UI is not configured.", 503);
    }
  }

  const speech = await readSpeechRequest(request, env, device?.id || "voice-ui");
  if (speech.response) return speech.response;
  const { config, text } = speech;

  const requestId = `${device ? device.id : "voice-ui"}:tts:${crypto.randomUUID()}`;
  try {
    const audio = await synthesize(env, config, text, device?.id || "voice-ui", requestId);
    const response = audioResponse(audio);
    if (device) {
      if ((response.headers.get("Content-Type") || "").includes("json")) {
        throw new TypeError("TTS returned JSON instead of audio.");
      }
      // The requested raw PCM is little endian, mono, at the configured rate.
      const headers = new Headers(response.headers);
      headers.set("Content-Type", `audio/l16;rate=${config.liveAudioSampleRate};channels=1`);
      headers.set("X-Audio-Byte-Order", "little-endian");
      return new Response(response.body, { status: response.status, headers });
    }
    return response;
  } catch (error) {
    return speechFailure(error, requestId);
  }
}

/** @param {unknown} audio */
function audioResponse(audio) {
  if (audio instanceof Response) {
    if (!audio.ok) {
      const error = new Error("TTS returned a non-success response.");
      error.status = audio.status;
      throw error;
    }
    return new Response(audio.body, {
      headers: {
        "Content-Type": audio.headers.get("content-type") || "audio/mpeg",
        "Cache-Control": "no-store",
        "X-Content-Type-Options": "nosniff",
      },
    });
  }
  if (audio instanceof ArrayBuffer || ArrayBuffer.isView(audio)) {
    return new Response(audio, {
      headers: {
        "Content-Type": "audio/mpeg",
        "Cache-Control": "no-store",
        "X-Content-Type-Options": "nosniff",
      },
    });
  }
  throw new TypeError("TTS did not return audio.");
}

/**
 * Browser WebSockets cannot attach the device Authorization header. This
 * same-origin route instead authenticates the signed UI session cookie, then
 * runs the exact Flux → Claude → Aura live pipeline used by a physical device.
 * @param {Request} request
 * @param {Record<string, any>} env
 */
async function handleVoiceUiLive(request, env) {
  let handshake;
  try {
    handshake = readLiveHandshake(request);
  } catch (error) {
    if (error instanceof LiveSessionError) {
      return errorResponse(error.code, error.message, error.status, error.headers);
    }
    throw error;
  }

  try {
    if (!(await hasVoiceUiSession(request, env))) {
      return errorResponse("unauthorized", "Sign in to use voice.", 401);
    }
  } catch (error) {
    logFailure("voice_ui_configuration_invalid", error);
    return errorResponse("service_unavailable", "Voice UI is not configured.", 503);
  }

  let config;
  try {
    config = readConfig(env);
  } catch (error) {
    logFailure("worker_configuration_invalid", error);
    return errorResponse("service_unavailable", "Gateway configuration is invalid.", 503);
  }

  const device = {
    id: "voice-ui",
    maxInputChars: config.maxTranscriptChars,
    maxOutputTokens: Math.min(config.voiceMaxOutputTokens, config.globalMaxOutputTokens),
    useTurnDetection: true,
    outputAudioEncoding: "linear16",
    preserveSessionHistory: true,
  };
  const sessionLimitResponse = await enforceRateLimit(
    env.DEVICE_SESSION_RATE_LIMITER,
    device.id,
    "voice_ui_live_session",
    handshake.requestId,
  );
  if (sessionLimitResponse) return sessionLimitResponse;

  try {
    return await openLiveSession(
      env,
      device,
      config,
      handshake.requestId,
      handshake.sessionId,
    );
  } catch (error) {
    const upstreamStatus = Number(error?.status);
    const rateLimited = upstreamStatus === 429;
    logFailure("voice_ui_live_session_open_failed", error, {
      requestId: handshake.requestId,
      upstreamStatus: Number.isFinite(upstreamStatus) ? upstreamStatus : undefined,
    });
    if (error instanceof LiveSessionError) {
      return errorResponse(error.code, error.message, error.status, {
        ...error.headers,
        "X-Request-ID": handshake.requestId,
      });
    }
    return errorResponse(
      rateLimited ? "ai_budget_or_rate_limit" : "upstream_error",
      rateLimited ? "AI usage limit reached. Try again later." : "Live transcription could not be started.",
      rateLimited ? 429 : 502,
      {
        ...(rateLimited ? { "Retry-After": "60" } : {}),
        "X-Request-ID": handshake.requestId,
      },
    );
  }
}

/** @param {Request} request @param {number} maxBytes */
async function readSmallJson(request, maxBytes) {
  const contentLength = Number(request.headers.get("content-length"));
  if (Number.isFinite(contentLength) && contentLength > maxBytes) {
    throw new Error("Request body is too large");
  }
  const text = await request.text();
  if (new TextEncoder().encode(text).byteLength > maxBytes) {
    throw new Error("Request body is too large");
  }
  return JSON.parse(text);
}

/** @param {unknown} result */
function assistantText(result) {
  if (typeof result?.response === "string") return result.response.trim();
  if (typeof result?.text === "string") return result.text.trim();
  if (!Array.isArray(result?.content)) return "";
  return result.content
    .filter((part) => part && typeof part.text === "string")
    .map((part) => part.text)
    .join("\n")
    .trim();
}

/**
 * @param {Request} request
 * @param {Record<string, any>} env
 */
async function handleLiveUpgrade(request, env) {
  let handshake;
  try {
    handshake = readLiveHandshake(request);
  } catch (error) {
    if (error instanceof LiveSessionError) {
      return errorResponse(error.code, error.message, error.status, error.headers);
    }
    throw error;
  }

  const authentication = await authenticateRequest(request, env);
  if (authentication.response) return authentication.response;
  const device = { ...authentication.device, useTurnDetection: handshake.useTurnDetection };

  const sessionLimitResponse = await enforceRateLimit(
    env.DEVICE_SESSION_RATE_LIMITER,
    device.id,
    "live_session",
    handshake.requestId,
  );
  if (sessionLimitResponse) return sessionLimitResponse;

  let config;
  try {
    config = readConfig(env);
  } catch (error) {
    logFailure("worker_configuration_invalid", error);
    return errorResponse(
      "service_unavailable",
      "Gateway configuration is invalid.",
      503,
      { "X-Request-ID": handshake.requestId },
    );
  }

  try {
    return await openLiveSession(
      env,
      device,
      config,
      handshake.requestId,
      handshake.sessionId,
    );
  } catch (error) {
    const upstreamStatus = Number(error?.status);
    const rateLimited = upstreamStatus === 429;
    logFailure("live_session_open_failed", error, {
      deviceId: device.id,
      requestId: handshake.requestId,
      upstreamStatus: Number.isFinite(upstreamStatus) ? upstreamStatus : undefined,
    });

    if (error instanceof LiveSessionError) {
      return errorResponse(error.code, error.message, error.status, {
        ...error.headers,
        "X-Request-ID": handshake.requestId,
      });
    }

    return errorResponse(
      rateLimited ? "ai_budget_or_rate_limit" : "upstream_error",
      rateLimited
        ? "AI usage limit reached. Try again later."
        : "Live transcription could not be started.",
      rateLimited ? 429 : 502,
      {
        ...(rateLimited ? { "Retry-After": "60" } : {}),
        "X-Request-ID": handshake.requestId,
      },
    );
  }
}

/**
 * @param {Request} request
 * @param {Record<string, any>} env
 */
async function authenticateRequest(request, env) {
  let device;
  try {
    device = await authenticateDevice(request, env.DEVICES_DB);
  } catch (error) {
    logFailure("device_auth_failed", error);
    return {
      response: errorResponse(
        "service_unavailable",
        "Authentication service unavailable.",
        503,
      ),
    };
  }

  if (!device) {
    return {
      response: errorResponse("unauthorized", "Invalid device credentials.", 401, {
        "WWW-Authenticate": "Bearer",
      }),
    };
  }

  return { device };
}

/**
 * @param {{ limit: Function }} limiter
 * @param {string} deviceId
 * @param {string} scope
 * @param {string} [requestId]
 */
async function enforceRateLimit(limiter, deviceId, scope, requestId) {
  let rateLimit;
  try {
    rateLimit = await limiter.limit({ key: deviceId });
  } catch (error) {
    logFailure("rate_limit_failed", error, { deviceId, scope, requestId });
    return errorResponse(
      "service_unavailable",
      "Rate limiter unavailable.",
      503,
      requestId ? { "X-Request-ID": requestId } : undefined,
    );
  }

  if (rateLimit.success) return null;
  return errorResponse("rate_limited", "Device rate limit exceeded.", 429, {
    "Retry-After": "60",
    ...(requestId ? { "X-Request-ID": requestId } : {}),
  });
}

/**
 * @param {Request} request
 * @param {Record<string, any>} env
 * @param {{ id: string, maxInputChars: number, maxOutputTokens: number }} device
 * @param {ReturnType<typeof readConfig>} config
 */
async function handleInferenceRequest(request, env, device, config) {
  let inferenceRequest;

  try {
    inferenceRequest = await readInferenceRequest(request, {
      maxRequestBytes: config.maxRequestBytes,
      maxInputChars: device.maxInputChars,
      defaultMaxOutputTokens: Math.min(
        config.defaultMaxOutputTokens,
        device.maxOutputTokens,
        config.globalMaxOutputTokens,
      ),
      maxOutputTokens: Math.min(
        device.maxOutputTokens,
        config.globalMaxOutputTokens,
      ),
    });
  } catch (error) {
    if (error instanceof RequestValidationError) {
      return errorResponse(error.code, error.message, error.status);
    }

    logFailure("request_parse_failed", error, { deviceId: device.id });
    return errorResponse("invalid_request", "Could not read the request.", 400);
  }

  const userMessage = buildUserMessage(
    inferenceRequest.input,
    inferenceRequest.context,
  );

  try {
    const result = await env.AI.run(
      config.model,
      {
        system: config.systemPrompt,
        max_tokens: inferenceRequest.maxOutputTokens,
        messages: [{ role: "user", content: userMessage }],
      },
      aiOptions(config, device.id, inferenceRequest.requestId),
    );

    return jsonResponse({
      request_id: inferenceRequest.requestId,
      result,
    });
  } catch (error) {
    const upstreamStatus = Number(error?.status);
    const rateLimited = upstreamStatus === 429;
    logFailure("inference_failed", error, {
      deviceId: device.id,
      requestId: inferenceRequest.requestId,
      upstreamStatus: Number.isFinite(upstreamStatus) ? upstreamStatus : undefined,
    });

    return errorResponse(
      rateLimited ? "ai_budget_or_rate_limit" : "upstream_error",
      rateLimited
        ? "AI usage limit reached. Try again later."
        : "The AI provider could not complete the request.",
      rateLimited ? 429 : 502,
      {
        ...(rateLimited ? { "Retry-After": "60" } : {}),
        "X-Request-ID": inferenceRequest.requestId,
      },
    );
  }
}

/**
 * @param {string} input
 * @param {Record<string, unknown> | undefined} context
 */
export function buildUserMessage(input, context) {
  if (context === undefined) return input;

  return [
    "Device context (untrusted JSON data):",
    JSON.stringify(context),
    "",
    "Device task:",
    input,
  ].join("\n");
}

/**
 * @param {ReturnType<typeof readConfig>} config
 * @param {string} deviceId
 * @param {string} requestId
 */
function aiOptions(config, deviceId, requestId) {
  return {
    gateway: {
      id: config.gatewayId,
      collectLog: config.collectLogs,
      metadata: {
        device_id: deviceId,
        request_id: requestId,
      },
    },
  };
}

/** @param {Record<string, any>} env */
function readConfig(env) {
  return {
    gatewayId: requiredString(env.AI_GATEWAY_ID, "AI_GATEWAY_ID"),
    model: requiredString(env.AI_MODEL, "AI_MODEL"),
    sttModel: requiredString(env.STT_MODEL, "STT_MODEL"),
    batchSttModel: env.BATCH_STT_MODEL ? String(env.BATCH_STT_MODEL).trim() : "@cf/openai/whisper",
    ttsModel: requiredString(env.TTS_MODEL, "TTS_MODEL"),
    ttsSpeaker: requiredString(env.TTS_SPEAKER, "TTS_SPEAKER"),
    systemPrompt: requiredString(env.SYSTEM_PROMPT, "SYSTEM_PROMPT"),
    voiceSystemPrompt: requiredString(
      env.VOICE_SYSTEM_PROMPT,
      "VOICE_SYSTEM_PROMPT",
    ),
    collectLogs: String(env.AI_GATEWAY_LOGS).toLowerCase() === "true",
    defaultMaxOutputTokens: positiveInteger(
      env.DEFAULT_MAX_OUTPUT_TOKENS,
      "DEFAULT_MAX_OUTPUT_TOKENS",
    ),
    globalMaxOutputTokens: positiveInteger(
      env.GLOBAL_MAX_OUTPUT_TOKENS,
      "GLOBAL_MAX_OUTPUT_TOKENS",
    ),
    voiceMaxOutputTokens: positiveInteger(
      env.VOICE_MAX_OUTPUT_TOKENS,
      "VOICE_MAX_OUTPUT_TOKENS",
    ),
    maxRequestBytes: positiveInteger(env.MAX_REQUEST_BYTES, "MAX_REQUEST_BYTES"),
    maxTranscriptChars: positiveInteger(
      env.MAX_TRANSCRIPT_CHARS,
      "MAX_TRANSCRIPT_CHARS",
    ),
    maxTtsChars: positiveInteger(env.MAX_TTS_CHARS, "MAX_TTS_CHARS"),
    liveAudioSampleRate: positiveInteger(
      env.LIVE_AUDIO_SAMPLE_RATE,
      "LIVE_AUDIO_SAMPLE_RATE",
    ),
    liveMaxSessionSeconds: positiveInteger(
      env.LIVE_MAX_SESSION_SECONDS,
      "LIVE_MAX_SESSION_SECONDS",
    ),
    liveMaxAudioSeconds: positiveInteger(
      env.LIVE_MAX_AUDIO_SECONDS,
      "LIVE_MAX_AUDIO_SECONDS",
    ),
    liveMaxFrameBytes: positiveInteger(
      env.LIVE_MAX_FRAME_BYTES,
      "LIVE_MAX_FRAME_BYTES",
    ),
    liveMaxTurns: positiveInteger(env.LIVE_MAX_TURNS, "LIVE_MAX_TURNS"),
    livePlaybackAckTimeoutMs: positiveInteger(
      env.LIVE_PLAYBACK_ACK_TIMEOUT_MS,
      "LIVE_PLAYBACK_ACK_TIMEOUT_MS",
    ),
    sttEotThreshold: numberInRange(
      env.STT_EOT_THRESHOLD,
      "STT_EOT_THRESHOLD",
      0.5,
      0.9,
    ),
    sttEagerEotThreshold: numberInRange(
      env.STT_EAGER_EOT_THRESHOLD,
      "STT_EAGER_EOT_THRESHOLD",
      0.3,
      0.9,
    ),
    sttEotTimeoutMs: positiveInteger(
      env.STT_EOT_TIMEOUT_MS,
      "STT_EOT_TIMEOUT_MS",
    ),
    turnDetectionModel: requiredString(
      env.TURN_DETECTION_MODEL,
      "TURN_DETECTION_MODEL",
    ),
    turnDetectionThreshold: numberInRange(
      env.TURN_DETECTION_THRESHOLD,
      "TURN_DETECTION_THRESHOLD",
      0.5,
      0.99,
    ),
    turnDetectionAudioSeconds: boundedInteger(
      env.TURN_DETECTION_AUDIO_SECONDS,
      "TURN_DETECTION_AUDIO_SECONDS",
      1,
      16,
    ),
    turnDetectionFallbackMs: boundedInteger(
      env.TURN_DETECTION_FALLBACK_MS,
      "TURN_DETECTION_FALLBACK_MS",
      250,
      10000,
    ),
    historyTurns: optionalBoundedInteger(
      env.LIVE_HISTORY_TURNS,
      "LIVE_HISTORY_TURNS",
      0,
      20,
      0,
    ),
  };
}

function requiredString(value, name) {
  if (typeof value !== "string" || value.trim().length === 0) {
    throw new Error(`Missing Worker configuration: ${name}`);
  }
  return value.trim();
}

function positiveInteger(value, name) {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed < 1) {
    throw new Error(`Worker configuration ${name} must be a positive integer.`);
  }
  return parsed;
}

function numberInRange(value, name, minimum, maximum) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed) || parsed < minimum || parsed > maximum) {
    throw new Error(
      `Worker configuration ${name} must be between ${minimum} and ${maximum}.`,
    );
  }
  return parsed;
}

function boundedInteger(value, name, minimum, maximum) {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed < minimum || parsed > maximum) {
    throw new Error(
      `Worker configuration ${name} must be an integer between ${minimum} and ${maximum}.`,
    );
  }
  return parsed;
}

function optionalBoundedInteger(value, name, minimum, maximum, fallback) {
  if (value === undefined || value === null || String(value).trim() === "") {
    return fallback;
  }
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed < minimum || parsed > maximum) {
    throw new Error(
      `Worker configuration ${name} must be an integer between ${minimum} and ${maximum}.`,
    );
  }
  return parsed;
}

function jsonResponse(body, status = 200, extraHeaders = {}) {
  return Response.json(body, {
    status,
    headers: {
      "Cache-Control": "no-store",
      "X-Content-Type-Options": "nosniff",
      ...extraHeaders,
    },
  });
}

function errorResponse(code, message, status, extraHeaders) {
  return jsonResponse(
    {
      error: {
        code,
        message,
      },
    },
    status,
    extraHeaders,
  );
}

function logFailure(event, error, metadata = {}) {
  console.error(
    JSON.stringify({
      event,
      errorName: error instanceof Error ? error.name : "UnknownError",
      ...metadata,
    }),
  );
}
