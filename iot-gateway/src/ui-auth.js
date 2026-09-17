const SESSION_TTL_SECONDS = 12 * 60 * 60;
const SESSION_VERSION = "v1";

/**
 * Checks the password used by the browser voice UI. The password is a Worker
 * secret in production and is read from .env by Wrangler during local dev.
 *
 * @param {string} candidate
 * @param {Record<string, any>} env
 */
export async function verifyVoiceUiPassword(candidate, env) {
  const configured = env.UI_PASSWORD;
  if (typeof configured !== "string" || configured.length < 12) {
    throw new Error("Missing or weak Worker secret: UI_PASSWORD");
  }

  return timingSafeEqual(
    new TextEncoder().encode(candidate),
    new TextEncoder().encode(configured),
  );
}

/**
 * @param {Request} request
 * @param {Record<string, any>} env
 */
export async function hasVoiceUiSession(request, env) {
  const configured = env.UI_PASSWORD;
  if (typeof configured !== "string" || configured.length < 12) {
    throw new Error("Missing or weak Worker secret: UI_PASSWORD");
  }

  const session = readCookie(request.headers.get("cookie"), "iot_voice_session");
  if (!session) return false;

  const [version, expiry, signature] = session.split(".");
  if (!version || !expiry || !signature || version !== SESSION_VERSION) return false;

  const expiresAt = Number(expiry);
  if (!Number.isSafeInteger(expiresAt) || expiresAt <= Math.floor(Date.now() / 1000)) {
    return false;
  }

  const expected = await signSession(`${version}.${expiry}`, configured);
  return timingSafeEqual(
    new TextEncoder().encode(signature),
    new TextEncoder().encode(expected),
  );
}

/** @param {Request} request @param {Record<string, any>} env */
export async function createVoiceUiSession(request, env) {
  const password = env.UI_PASSWORD;
  if (typeof password !== "string" || password.length < 12) {
    throw new Error("Missing or weak Worker secret: UI_PASSWORD");
  }

  const expiry = String(Math.floor(Date.now() / 1000) + SESSION_TTL_SECONDS);
  const payload = `${SESSION_VERSION}.${expiry}`;
  const signature = await signSession(payload, password);
  const secure = isLocalRequest(request) ? "" : "; Secure";
  return `iot_voice_session=${payload}.${signature}; Path=/; Max-Age=${SESSION_TTL_SECONDS}; HttpOnly; SameSite=Strict${secure}`;
}

/** @param {Request} request */
export function clearVoiceUiSession(request) {
  const secure = isLocalRequest(request) ? "" : "; Secure";
  return `iot_voice_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict${secure}`;
}

/** @param {string} value @param {string} password */
async function signSession(value, password) {
  const key = await crypto.subtle.importKey(
    "raw",
    new TextEncoder().encode(password),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  const signature = await crypto.subtle.sign(
    "HMAC",
    key,
    new TextEncoder().encode(value),
  );
  return base64Url(new Uint8Array(signature));
}

/** @param {string | null} header @param {string} name */
function readCookie(header, name) {
  if (!header) return null;
  const prefix = `${name}=`;
  for (const part of header.split(";")) {
    const value = part.trim();
    if (value.startsWith(prefix)) return value.slice(prefix.length);
  }
  return null;
}

/** @param {Uint8Array} bytes */
function base64Url(bytes) {
  let value = "";
  for (const byte of bytes) value += String.fromCharCode(byte);
  return btoa(value).replaceAll("+", "-").replaceAll("/", "_").replaceAll("=", "");
}

/** @param {Uint8Array} left @param {Uint8Array} right */
function timingSafeEqual(left, right) {
  const length = Math.max(left.length, right.length);
  let difference = left.length ^ right.length;
  for (let index = 0; index < length; index += 1) {
    difference |= (left[index] ?? 0) ^ (right[index] ?? 0);
  }
  return difference === 0;
}

/** @param {Request} request */
function isLocalRequest(request) {
  const hostname = new URL(request.url).hostname;
  return hostname === "localhost" || hostname === "127.0.0.1" || hostname === "::1";
}
