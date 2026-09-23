#pragma once

#include <stdint.h>

#include "Emotion.h"

class Display;

// Bunty's speaking face, rendered pixel by pixel into the Flow32 framebuffer
// from signed distance fields: every edge is anti-aliased and every shape can
// tween smoothly into the next emotion's.
//
// Eyes are glowing rounded shapes cut by eyelids (slanted for anger or
// sadness, arced from below for a happy squint), with a shine, and they blink
// and glance on their own. Each emotion adds its own touches: blush, a falling
// tear, sparkles, a question mark. The mouth is a lip line that bends into a
// smile, frown or smirk and opens with speech, showing a tongue or gritted
// teeth, or becomes a round "o".
//
// Only depends on Display, so the host preview can render it too.
class Face {
 public:
  static constexpr int16_t kEyesTop = 92;
  static constexpr int16_t kEyesHeight = 146;
  static constexpr int16_t kMouthLeft = 40;
  static constexpr int16_t kMouthTop = 238;
  static constexpr int16_t kMouthWidth = 160;
  static constexpr int16_t kMouthHeight = 58;

  // The pixels a draw call touched; only these need presenting.
  struct Rect {
    int16_t x0, y0, x1, y1;
  };

  // Jumps straight to the current emotion, without a tween.
  // The next draws repaint their whole bands.
  void reset(uint32_t now);
  void setEmotion(Emotion emotion) { emotion_ = emotion; }

  // Each repaints, without presenting, only what changed in its band: the
  // features this frame plus wherever they were last frame. Everything else
  // is left as the background it already is.
  Rect drawEyes(Display &display, uint32_t now, uint16_t background);
  // `open` is 0 (closed) to 1 (as wide as this emotion's mouth goes).
  Rect drawMouth(Display &display, float open, uint32_t now, uint16_t background);

  struct Eye {
    float width, height, round;  // round: 0 square .. 1 fully rounded
    float dx, dy;                // offset; dx is outward from the face centre
    float lid;                   // upper lid cover, fraction of height
    float slant;                 // lid slope: + lowers the inner corner (anger)
    float cut;                   // happy squint: lower arc cut, fraction of height
    float shine;                 // highlight size multiplier
  };
  struct Mouth {
    float width, curve;  // curve: + smile, - frown, px
    float rest, gain;    // opening at silence, and added at full speech
    float depth;         // px the mouth opens at full
    float asym;          // + lifts the right corner (smirk)
    float teeth, tongue, ring;
    // Lens mouths open as a gap between the lips; the others are a drawn
    // stroke with rounded ends that opens into an oval.
    enum Shape : uint8_t { kLens, kSmile, kShy, kSmirk } shape;
  };
  struct Extras {
    float blush, tear, sparkle, question;
  };

 private:
  void advance(uint32_t now);
  Rect drawStrokeMouth(Display &display, float opening, uint16_t background);
  float random();

  Emotion emotion_ = Emotion::Neutral;
  Eye eyes_[2] = {};
  Mouth mouth_ = {};
  Extras extras_ = {};
  float gazeX_ = 0.0f, gazeY_ = 0.0f, gazeTargetX_ = 0.0f, gazeTargetY_ = 0.0f;
  Rect lastEyes_ = {};
  float lastEyesSignature_[21] = {};
  float lastMouthOpening_ = -1.0f;
  Rect lastMouth_ = {};
  uint32_t lastAdvanceAt_ = 0;
  uint32_t lastMouthAt_ = 0;
  uint32_t nextBlinkAt_ = 0;
  uint32_t blinkStartedAt_ = 0;
  uint32_t nextGlanceAt_ = 0;
  uint32_t seed_ = 0x9e3779b9u;
  bool blinking_ = false;
};
