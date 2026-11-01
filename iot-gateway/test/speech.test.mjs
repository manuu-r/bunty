import assert from "node:assert/strict";
import test from "node:test";

import { speakable, speechSegments } from "../src/speech.js";

test("each tagged sentence becomes a segment with its own expression", () => {
  assert.deepEqual(
    speechSegments("[curious] Wait, you built me a head? [happy] [nod] Honestly, it suits me."),
    [
      { emotion: "curious", gesture: null, picture: "search", text: "Wait, you built me a head?" },
      { emotion: "happy", gesture: "nod", picture: null, text: "Honestly, it suits me." },
    ],
  );
});

test("unknown bracket words are dropped rather than spoken", () => {
  assert.deepEqual(speechSegments("[grumpy] Fine, I will do it myself."), [
    { emotion: "neutral", gesture: null, picture: null, text: "Fine, I will do it myself." },
  ]);
});

test("untagged replies still get expression from their punctuation", () => {
  const segments = speechSegments("Did that really work? It genuinely did work! The tests agree with me.");
  assert.deepEqual(segments.map((segment) => segment.emotion), ["curious", "excited", "excited"]);
});

test("fragments and decimals do not split sentences", () => {
  assert.deepEqual(
    speechSegments("Dr. Rao measured 3.5 volts on the rail today.").map((segment) => segment.text),
    ["Dr. Rao measured 3.5 volts on the rail today."],
  );
});

test("a long reply folds into a bounded number of synthesis calls", () => {
  const reply = Array.from({ length: 30 }, (_, index) => `[happy] Sentence number ${index} is here.`);
  const segments = speechSegments(reply.join(" "));
  assert.equal(segments.length, 8);
  assert.match(segments.at(-1).text, /Sentence number 29 is here\.$/);
});

test("markdown, links and emoji never reach the synthesizer", () => {
  assert.equal(
    speakable("**Bold** and _soft_ `code` [the docs](https://example.com/a) \u{1F600} https://example.com/b").replace(/\s+/g, " ").trim(),
    "Bold and soft code the docs",
  );
  assert.equal(speakable("keep snake_case words"), "keep snake_case words");
  assert.deepEqual(
    speechSegments("# Plan\n- First item here today\n- Second item here today").map((segment) => segment.text),
    ["Plan First item here today", "Second item here today"],
  );
});

test("removing an emoji leaves no gap before the punctuation", () => {
  assert.equal(speechSegments("Honestly it suits me \u{1F600}.")[0].text, "Honestly it suits me.");
});

test("a picture tag lands on its sentence and displaces the emotion's picture", () => {
  assert.deepEqual(
    speechSegments("[happy] Good morning there friend. [neutral] [rain] Bring an umbrella today.")
      .map((segment) => segment.picture),
    [null, "rain"],
  );
});

test("text with nothing speakable produces no segments", () => {
  assert.deepEqual(speechSegments("[happy] \u{1F600} ..."), []);
});
