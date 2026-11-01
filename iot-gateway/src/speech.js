import { EMOTIONS } from "./live.js";
import { PICTURE_NAMES, pictureForEmotion, pictureGif } from "./pictures.js";

// Tags Bunty's firmware acts on. Any other bracketed word is dropped, so a
// speech synthesizer never reads "bracket grumpy bracket" aloud.
const GESTURES = ["nod", "shake"];
const TAG = /\[\s*([A-Za-z]+)\s*\]/;

// Each segment is one Aura call. Fragments like "Dr." join the next sentence,
// and a long reply folds into its last segment rather than fanning out.
const MIN_SEGMENT_CHARS = 16;
const MAX_SEGMENTS = 8;

// Binary frames: one type byte, a 24-bit little-endian length, then the payload.
export const FRAME_AUDIO = 1;
export const FRAME_CUE = 2;
export const FRAME_END = 3;
export const FRAME_PICTURE = 4; // a GIF for the cue that follows it

/** Remove what reads badly aloud; bracket tags and line breaks survive. */
export function speakable(text) {
  return text
    .replace(/```[\s\S]*?```/g, " ")
    .replace(/`([^`]*)`/g, "$1")
    .replace(/!?\[([^\]]*)\]\([^)]*\)/g, "$1")
    .replace(/https?:\/\/\S+/g, " ")
    .replace(/^\s*(?:#{1,6}\s+|>\s+|[-*+]\s+|\d+[.)]\s+)/gm, "")
    .replace(/\*+|~~|(?<![A-Za-z0-9])_+|_+(?![A-Za-z0-9])/g, "")
    .replace(/\p{Extended_Pictographic}|️/gu, "");
}

/** @param {string} text */
function sentences(text) {
  const parts = [];
  let start = 0;
  for (const match of text.matchAll(/[.!?…]+["')]*(?=\s|$)|\n+/g)) {
    const end = match.index + match[0].length;
    parts.push(text.slice(start, end));
    start = end;
  }
  parts.push(text.slice(start));
  return parts
    .map((part) => part.replace(/\s+/g, " ").replace(/ ([.,!?…])/g, "$1").trim())
    .filter((part) => /[\p{L}\p{N}]/u.test(part));
}

// Muse is not obliged to tag every sentence; punctuation still gives it a pulse.
function inferredEmotion(sentence) {
  if (sentence.endsWith("?")) return "curious";
  if (sentence.endsWith("!")) return "excited";
  return null;
}

/**
 * Split tagged reply text into spoken segments, each with the expression to
 * perform at the moment its first sample plays.
 *
 * @param {string} text
 * @returns {{ emotion: string, gesture: string | null, picture: string | null, text: string }[]}
 */
export function speechSegments(text) {
  const segments = [];
  let emotion = null;
  let gesture = null;
  let picture = null;

  speakable(text).split(TAG).forEach((part, index) => {
    if (index % 2 === 1) {
      const name = part.toLowerCase();
      if (EMOTIONS.includes(name)) emotion = name;
      else if (GESTURES.includes(name)) gesture = name;
      else if (PICTURE_NAMES.includes(name)) picture = name;
      return;
    }
    for (const sentence of sentences(part)) {
      const previous = segments.at(-1);
      const tagged = emotion !== null || gesture !== null || picture !== null;
      if (
        previous &&
        (segments.length >= MAX_SEGMENTS ||
          (!tagged && previous.text.length < MIN_SEGMENT_CHARS))
      ) {
        previous.text += ` ${sentence}`;
      } else {
        segments.push({
          emotion:
            emotion ?? inferredEmotion(sentence) ?? previous?.emotion ?? "neutral",
          gesture,
          picture,
          text: sentence,
        });
      }
      emotion = null;
      gesture = null;
      picture = null;
    }
  });

  // A reply Muse left without a picture still shows the feeling it opens with.
  if (!segments.some((segment) => segment.picture)) {
    const felt = segments.find((segment) => pictureForEmotion(segment.emotion));
    if (felt) felt.picture = pictureForEmotion(felt.emotion);
  }
  return segments;
}

/** @param {number} type @param {Uint8Array} payload */
export function speechFrame(type, payload) {
  const frame = new Uint8Array(4 + payload.byteLength);
  frame[0] = type;
  frame[1] = payload.byteLength & 0xff;
  frame[2] = (payload.byteLength >> 8) & 0xff;
  frame[3] = (payload.byteLength >> 16) & 0xff;
  frame.set(payload, 4);
  return frame;
}

/** @param {{ emotion: string, gesture: string | null }} segment */
function cueFrame(segment) {
  return speechFrame(
    FRAME_CUE,
    new TextEncoder().encode(`${segment.emotion},${segment.gesture ?? ""}`),
  );
}

/** @param {number} audioBytes */
function endFrame(audioBytes) {
  const payload = new Uint8Array(4);
  new DataView(payload.buffer).setUint32(0, audioBytes, true);
  return speechFrame(FRAME_END, payload);
}

/**
 * Audio already played is worth keeping, so a failure past the first segment
 * ends the stream early instead of erroring it.
 *
 * @param {ReturnType<typeof speechSegments>} segments
 * @param {ReadableStream} firstBody
 * @param {(text: string) => Promise<ReadableStream>} synthesize
 * @param {(error: unknown) => void} onFailure
 */
async function* speechFrames(segments, firstBody, synthesize, onFailure) {
  let audioBytes = 0;
  let pending = Promise.resolve({ body: firstBody });
  try {
    for (let index = 0; index < segments.length; index += 1) {
      const { body, error } = await pending;
      // Synthesize one sentence ahead, so the next is ready as this one ends.
      // The result is settled here and only inspected on its turn.
      pending =
        index + 1 < segments.length
          ? synthesize(segments[index + 1].text).then(
              (next) => ({ body: next }),
              (failure) => ({ error: failure }),
            )
          : null;
      if (error) throw error;

      if (segments[index].picture) {
        yield speechFrame(FRAME_PICTURE, pictureGif(segments[index].picture));
      }
      yield cueFrame(segments[index]);
      const reader = body.getReader();
      try {
        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          if (value.byteLength === 0) continue;
          audioBytes += value.byteLength;
          yield speechFrame(FRAME_AUDIO, value);
        }
      } finally {
        await reader.cancel().catch(() => {});
      }
    }
  } catch (error) {
    onFailure(error);
  } finally {
    pending?.then((result) => result.body?.cancel()).catch(() => {});
  }
  yield endFrame(audioBytes);
}

/**
 * The cues travel inside the audio stream, at the byte they apply to, so sound,
 * face and head share the device's playback position as their only clock.
 *
 * @param {ReturnType<typeof speechSegments>} segments
 * @param {ReadableStream} firstBody already synthesized `segments[0]`
 * @param {(text: string) => Promise<ReadableStream>} synthesize
 * @param {(error: unknown) => void} onFailure
 */
export function speechStream(segments, firstBody, synthesize, onFailure) {
  const frames = speechFrames(segments, firstBody, synthesize, onFailure);
  return new ReadableStream({
    async pull(controller) {
      const { done, value } = await frames.next();
      if (done) controller.close();
      else controller.enqueue(value);
    },
    async cancel() {
      await frames.return();
    },
  });
}
