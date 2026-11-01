import { authenticateDevice } from "./auth.js";
import {
  clearVoiceUiSession,
  createVoiceUiSession,
  hasVoiceUiSession,
  verifyVoiceUiPassword,
} from "./ui-auth.js";
import {
  LiveSessionError,
  openLiveSession,
  readLiveHandshake,
  splitEmotion,
} from "./live.js";
import { readInferenceRequest, RequestValidationError } from "./request.js";
import { voiceLoginPage, voiceUiPage } from "./voice-ui.js";

const INFERENCE_ROUTE = "/v1/infer";
const LIVE_ROUTE = "/v1/live";
const VOICE_LOGIN_ROUTE = "/v1/voice/login";
const VOICE_LOGOUT_ROUTE = "/v1/voice/logout";
const VOICE_CHAT_ROUTE = "/v1/voice/chat";
const VOICE_SPEAK_ROUTE = "/v1/voice/speak";
const VOICE_LIVE_ROUTE = "/v1/voice/live";

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

  const rateLimitResponse = await enforceRateLimit(
    env.DEVICE_RATE_LIMITER,
    device?.id || "voice-ui",
    "tts",
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
    return errorResponse("invalid_request", "Could not read the speech request.", 400);
  }
  const text = typeof body.text === "string" ? body.text.trim() : "";
  if (!text || text.length > config.maxTtsChars) {
    return errorResponse(
      "invalid_request",
      `Speech must be between 1 and ${config.maxTtsChars} characters.`,
      400,
    );
  }

  const requestId = `${device ? device.id : "voice-ui"}:tts:${crypto.randomUUID()}`;
  try {
    const audio = await env.AI.run(
      config.ttsModel,
      {
        text,
        speaker: config.ttsSpeaker,
        encoding: "linear16",
        container: "none",
        sample_rate: config.liveAudioSampleRate,
      },
      { returnRawResponse: true, ...aiOptions(config, device?.id || "voice-ui", requestId) },
    );
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
