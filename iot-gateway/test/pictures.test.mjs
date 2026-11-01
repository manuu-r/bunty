import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

import { PICTURE_NAMES, pictureForEmotion, pictureGif } from "../src/pictures.js";

// The device reserves this much per picture; see firmware/src/TtsStream.h.
const DEVICE_LIMIT_BYTES = 2048;

test("every picture is a looping GIF the device will accept and zoom exactly", () => {
  for (const name of PICTURE_NAMES) {
    const gif = pictureGif(name);
    const text = new TextDecoder("latin1").decode(gif);
    assert.equal(text.slice(0, 6), "GIF89a", name);
    assert.equal(gif[6] | (gif[7] << 8), 48, name);
    assert.equal(gif[8] | (gif[9] << 8), 64, name);
    assert.ok(text.includes("NETSCAPE2.0"), name);
    assert.equal(gif.at(-1), 0x3b, name);
    assert.ok(gif.length <= DEVICE_LIMIT_BYTES, `${name} is ${gif.length} bytes`);
  }
});

test("pictures stay very small, each and in total", () => {
  const sizes = PICTURE_NAMES.map((name) => pictureGif(name).length);
  assert.ok(Math.max(...sizes) < 1024);
  assert.ok(sizes.reduce((sum, size) => sum + size, 0) < 16384);
});

test("an unknown picture is absent rather than an error", () => {
  assert.equal(pictureGif("constructor"), null);
});

test("every emotion's picture exists", () => {
  for (const emotion of ["excited", "happy", "soft", "sad", "angry", "curious"]) {
    assert.ok(PICTURE_NAMES.includes(pictureForEmotion(emotion)), emotion);
  }
  assert.equal(pictureForEmotion("neutral"), null);
});

test("Muse is offered exactly the pictures that exist", () => {
  const adapter = readFileSync(new URL("../../firmware/scripts/adapt_muse.py", import.meta.url), "utf8");
  const offered = adapter.match(/one picture tag[^.]*?: ((?:\[[a-z]+\] ?)+)/);
  assert.ok(offered, "the voice prompt should list picture tags");
  assert.deepEqual(
    offered[1].trim().split(" ").map((tag) => tag.slice(1, -1)).sort(),
    [...PICTURE_NAMES].sort(),
  );
});
