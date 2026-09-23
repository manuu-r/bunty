// Per-pixel float work: worth optimizing for speed, not the build's -Os.
#pragma GCC optimize("O2")

#include "Face.h"

#include <math.h>

#include "flow32/graphics/Display.h"

namespace {

struct Rgb {
  float r, g, b;
};

constexpr Rgb rgb(uint8_t r, uint8_t g, uint8_t b) { return {r / 255.0f, g / 255.0f, b / 255.0f}; }

// Indexed by Emotion.
constexpr Rgb kColors[] = {
    rgb(40, 230, 230),   // neutral: cyan
    rgb(190, 160, 255),  // soft: lavender
    rgb(255, 210, 60),   // happy: sunshine
    rgb(255, 130, 40),   // excited: orange
    rgb(255, 55, 55),    // angry: red
    rgb(70, 140, 255),   // sad: blue
    rgb(80, 240, 140),   // curious: green
    rgb(255, 80, 200),   // sassy: magenta
};
constexpr Rgb kWhite = rgb(255, 255, 255);
constexpr Rgb kBlush = rgb(255, 110, 150);
constexpr Rgb kTear = rgb(150, 210, 255);
constexpr Rgb kTongue = rgb(255, 105, 130);

using Eye = Face::Eye;
using Mouth = Face::Mouth;
using Extras = Face::Extras;

// {width, height, round, dx, dy, lid, slant, arc, shine}; left eye, then right.
constexpr Eye kEyes[][2] = {
    {{46, 62, 0.80f, 0, 0, 0.00f, 0.00f, 0.00f, 1.0f},  // neutral
     {46, 62, 0.80f, 0, 0, 0.00f, 0.00f, 0.00f, 1.0f}},
    {{50, 54, 0.90f, 0, 4, 0.42f, -0.12f, 0.00f, 0.8f},  // soft: heavy lids
     {50, 54, 0.90f, 0, 4, 0.42f, -0.12f, 0.00f, 0.8f}},
    {{56, 60, 1.00f, 0, -2, 0.00f, 0.00f, 1.00f, 0.0f},  // happy: ^ ^ arcs
     {56, 60, 1.00f, 0, -2, 0.00f, 0.00f, 1.00f, 0.0f}},
    {{62, 74, 0.85f, 0, -6, 0.00f, 0.00f, 0.00f, 1.3f},  // excited: huge
     {62, 74, 0.85f, 0, -6, 0.00f, 0.00f, 0.00f, 1.3f}},
    {{54, 46, 0.30f, -2, 8, 0.40f, 0.60f, 0.00f, 0.0f},  // angry: glare
     {54, 46, 0.30f, -2, 8, 0.40f, 0.60f, 0.00f, 0.0f}},
    {{50, 60, 0.85f, 2, 10, 0.30f, -0.45f, 0.00f, 1.6f},  // sad: droop, glossy
     {50, 60, 0.85f, 2, 10, 0.30f, -0.45f, 0.00f, 1.6f}},
    {{56, 72, 0.85f, 0, -8, 0.00f, 0.00f, 0.00f, 1.2f},  // curious: one eye wide
     {42, 46, 0.85f, 2, 6, 0.25f, 0.00f, 0.00f, 0.9f}},
    {{52, 58, 0.80f, 0, 2, 0.52f, 0.00f, 0.00f, 0.8f},  // sassy: unimpressed lid
     {52, 62, 0.80f, 0, -4, 0.00f, 0.00f, 0.00f, 0.9f}},
};

// {width, curve, rest, gain, depth, asym, teeth, tongue, ring, shape}
constexpr Mouth kMouths[] = {
    {62, 10, 0.00f, 1.00f, 24, 0, 0, 0, 0, Mouth::kSmile},   // neutral: easy smile
    {34, 7, 0.00f, 1.00f, 12, 0, 0, 1, 0, Mouth::kShy},      // soft: shy smile, opens to an o
    {70, 14, 0.25f, 0.90f, 20, 0, 0, 1, 0, Mouth::kLens},    // happy: grin with tongue
    {80, 16, 0.55f, 0.60f, 22, 0, 0, 1, 0, Mouth::kLens},    // excited: big open grin
    {62, -7, 0.35f, 0.65f, 14, 0, 1, 0, 0, Mouth::kLens},    // angry: gritted teeth
    {48, -12, 0.00f, 0.60f, 12, 0, 0, 0, 0, Mouth::kLens},   // sad: frown
    {20, 0, 0.30f, 0.70f, 10, 0, 0, 0, 1, Mouth::kLens},     // curious: "o"
    {60, 0, 0.00f, 0.80f, 18, 14, 0, 0, 0, Mouth::kSmirk},   // sassy: lopsided smirk
};

// {blush, tear, sparkle, question}
constexpr Extras kExtras[] = {
    {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {0, 0, 1, 0},
    {0, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 0},
};

constexpr size_t kEmotions = static_cast<size_t>(Emotion::Count);
static_assert(sizeof(kColors) / sizeof(kColors[0]) == kEmotions, "colour per emotion");
static_assert(sizeof(kEyes) / sizeof(kEyes[0]) == kEmotions, "eyes per emotion");
static_assert(sizeof(kMouths) / sizeof(kMouths[0]) == kEmotions, "mouth per emotion");
static_assert(sizeof(kExtras) / sizeof(kExtras[0]) == kEmotions, "extras per emotion");

constexpr float kLeftEyeX = 72.0f;
constexpr float kRightEyeX = 168.0f;
constexpr float kEyeY = 162.0f;
constexpr float kMouthX = 120.0f;
constexpr float kMouthY = 260.0f;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float mixf(float a, float b, float t) { return a + (b - a) * t; }
float length(float x, float y) { return sqrtf(x * x + y * y); }
// Coverage of a pixel by a shape whose signed distance there is `d`.
float cover(float d) { return clampf(0.5f - d, 0.0f, 1.0f); }

Rgb mix(const Rgb &a, const Rgb &b, float t) {
  return {mixf(a.r, b.r, t), mixf(a.g, b.g, t), mixf(a.b, b.b, t)};
}
Rgb scale(const Rgb &c, float k) {
  return {clampf(c.r * k, 0, 1), clampf(c.g * k, 0, 1), clampf(c.b * k, 0, 1)};
}
Rgb from565(uint16_t c) {
  return {((c >> 11) & 31) / 31.0f, ((c >> 5) & 63) / 63.0f, (c & 31) / 31.0f};
}
uint16_t to565(const Rgb &c) {
  return static_cast<uint16_t>((uint16_t(c.r * 31.0f + 0.5f) << 11) |
                               (uint16_t(c.g * 63.0f + 0.5f) << 5) |
                               uint16_t(c.b * 31.0f + 0.5f));
}

// Distance to a segment, for capsule strokes.
float sdSegment(float x, float y, float ax, float ay, float bx, float by) {
  const float px = x - ax, py = y - ay, vx = bx - ax, vy = by - ay;
  const float h = clampf((px * vx + py * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
  return length(px - vx * h, py - vy * h);
}

// A four-point sparkle: two crossed thin diamonds.
float sdSparkle(float x, float y, float size) {
  const float ax = fabsf(x), ay = fabsf(y);
  const float thin = size * 0.28f;
  const float a = (ax / thin + ay / size - 1.0f) * thin * 0.8f;
  const float b = (ax / size + ay / thin - 1.0f) * thin * 0.8f;
  return fminf(a, b);
}

void tween(float &value, float goal, float k) { value += (goal - value) * k; }

void tween(Eye &eye, const Eye &goal, float k) {
  tween(eye.width, goal.width, k);
  tween(eye.height, goal.height, k);
  tween(eye.round, goal.round, k);
  tween(eye.dx, goal.dx, k);
  tween(eye.dy, goal.dy, k);
  tween(eye.lid, goal.lid, k);
  tween(eye.slant, goal.slant, k);
  tween(eye.cut, goal.cut, k);
  tween(eye.shine, goal.shine, k);
}

void tween(Mouth &mouth, const Mouth &goal, float k) {
  tween(mouth.width, goal.width, k);
  tween(mouth.curve, goal.curve, k);
  tween(mouth.rest, goal.rest, k);
  tween(mouth.gain, goal.gain, k);
  tween(mouth.depth, goal.depth, k);
  tween(mouth.asym, goal.asym, k);
  tween(mouth.teeth, goal.teeth, k);
  tween(mouth.tongue, goal.tongue, k);
  tween(mouth.ring, goal.ring, k);
}

void tween(Extras &extras, const Extras &goal, float k) {
  tween(extras.blush, goal.blush, k);
  tween(extras.tear, goal.tear, k);
  tween(extras.sparkle, goal.sparkle, k);
  tween(extras.question, goal.question, k);
}

// Fraction of the way to a goal after `ms`, for a time constant of `tauMs`.
float approach(uint32_t ms, float tauMs) { return 1.0f - expf(-static_cast<float>(ms) / tauMs); }

}  // namespace

namespace {

using Rect = Face::Rect;

Rect unite(const Rect &a, const Rect &b) {
  if (a.x1 <= a.x0 || a.y1 <= a.y0) return b;
  if (b.x1 <= b.x0 || b.y1 <= b.y0) return a;
  return {a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
          a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1};
}

Rect clip(Rect r, int16_t left, int16_t top, int16_t width, int16_t height) {
  if (r.x0 < left) r.x0 = left;
  if (r.y0 < top) r.y0 = top;
  if (r.x1 > left + width) r.x1 = left + width;
  if (r.y1 > top + height) r.y1 = top + height;
  return r;
}

Rect around(float x0, float y0, float x1, float y1) {
  return {static_cast<int16_t>(floorf(x0)), static_cast<int16_t>(floorf(y0)),
          static_cast<int16_t>(ceilf(x1)), static_cast<int16_t>(ceilf(y1))};
}

}  // namespace

float Face::random() {
  seed_ = seed_ * 1664525u + 1013904223u;
  return (seed_ >> 8) / 16777216.0f;
}

void Face::reset(uint32_t now) {
  const uint8_t mood = static_cast<uint8_t>(emotion_);
  eyes_[0] = kEyes[mood][0];
  eyes_[1] = kEyes[mood][1];
  mouth_ = kMouths[mood];
  extras_ = kExtras[mood];
  gazeX_ = gazeY_ = gazeTargetX_ = gazeTargetY_ = 0.0f;
  lastAdvanceAt_ = lastMouthAt_ = now;
  nextBlinkAt_ = now + 1500;
  lastEyes_ = {0, kEyesTop, 240, static_cast<int16_t>(kEyesTop + kEyesHeight)};
  // Guarantees the first frame after a reset differs, so it paints.
  lastEyesSignature_[0] = -1e6f;
  lastMouthOpening_ = -1.0f;
  lastMouth_ = {kMouthLeft, kMouthTop, static_cast<int16_t>(kMouthLeft + kMouthWidth),
                static_cast<int16_t>(kMouthTop + kMouthHeight)};
  nextGlanceAt_ = now + 1200;
  blinking_ = false;
}

void Face::advance(uint32_t now) {
  const uint32_t elapsed = now - lastAdvanceAt_;
  lastAdvanceAt_ = now;
  const uint8_t mood = static_cast<uint8_t>(emotion_);
  const float k = approach(elapsed, 90.0f);
  tween(eyes_[0], kEyes[mood][0], k);
  tween(eyes_[1], kEyes[mood][1], k);
  tween(extras_, kExtras[mood], approach(elapsed, 200.0f));

  if (static_cast<int32_t>(now - nextGlanceAt_) >= 0) {
    gazeTargetX_ = (random() - 0.5f) * 14.0f;
    gazeTargetY_ = (random() - 0.5f) * 8.0f;
    nextGlanceAt_ = now + 900 + static_cast<uint32_t>(random() * 2200.0f);
  }
  // Glances are quick saccades, not drifts.
  const float g = approach(elapsed, 45.0f);
  tween(gazeX_, gazeTargetX_, g);
  tween(gazeY_, gazeTargetY_, g);

  if (!blinking_ && static_cast<int32_t>(now - nextBlinkAt_) >= 0) {
    blinking_ = true;
    blinkStartedAt_ = now;
  }
}

namespace {

// Everything about one eye that is constant for a frame, so the per-pixel
// loop is a handful of multiplies and, only near curves, a square root.
struct EyeFrame {
  float cx, cy, hx, hy, radius;
  bool lidded;
  float lidTop, lidSlope, lidNorm;  // lid line y = lidTop + lidSlope * x
  float arc;  // 0 an ordinary eye .. 1 a happy ^ arc; between, a morph
  float arcRadius, arcCentre, arcEndX, arcEndY;
  float shine;
  float shineX, shineY, shineR, glintX, glintY, glintR;
  int16_t x0, x1, y0, y1;
};

EyeFrame frameFor(const Face::Eye &eye, float cx, float cy, float inner, float blink) {
  EyeFrame f = {};
  f.cx = cx;
  f.cy = cy;
  f.hx = eye.width * 0.5f;
  f.hy = eye.height * 0.5f;
  f.radius = eye.round * fminf(f.hx, f.hy);
  // A blink lowers the lid most of the way; a sliver stays so it reads as a
  // blink rather than the eye vanishing.
  const float lid = mixf(eye.lid, 0.92f, blink);
  const float slant = eye.slant * (1.0f - blink);
  f.lidded = lid > 0.001f || fabsf(slant) > 0.001f;
  f.lidTop = -f.hy + lid * eye.height;
  f.lidSlope = slant * inner;
  f.lidNorm = 1.0f / sqrtf(1.0f + slant * slant);
  f.arc = clampf(eye.cut, 0.0f, 1.0f);
  f.arcRadius = f.hx * 0.85f;
  f.arcCentre = f.arcRadius * 0.45f;
  // The arc's ends sit 25 degrees above horizontal.
  f.arcEndX = f.arcRadius * 0.906f;
  f.arcEndY = f.arcCentre - f.arcRadius * 0.423f;
  f.shine = eye.shine;
  const float size = fminf(eye.width, eye.height);
  // Shine sits in the part of the eye the lid leaves visible.
  const float drop = eye.lid * eye.height * 0.55f;
  f.shineX = -eye.width * 0.2f;
  f.shineY = -eye.height * 0.22f + drop;
  f.shineR = size * 0.14f * eye.shine;
  f.glintX = eye.width * 0.18f;
  f.glintY = eye.height * 0.2f;
  f.glintR = size * 0.065f * eye.shine;
  constexpr float kGlow = 13.0f;
  f.x0 = static_cast<int16_t>(cx - f.hx - kGlow);
  f.x1 = static_cast<int16_t>(cx + f.hx + kGlow + 1);
  f.y0 = static_cast<int16_t>(cy - f.hy - kGlow);
  f.y1 = static_cast<int16_t>(cy + f.hy + kGlow + 1);
  return f;
}

// sdRoundBox with the square root confined to the rounded corners.
float sdEyeFast(const EyeFrame &f, float x, float y) {
  const float qx = fabsf(x) - f.hx + f.radius;
  const float qy = fabsf(y) - f.hy + f.radius;
  float d;
  if (qx > 0.0f && qy > 0.0f) d = sqrtf(qx * qx + qy * qy) - f.radius;
  else d = fmaxf(qx, qy) - f.radius;
  if (f.lidded) d = fmaxf(d, (f.lidTop + f.lidSlope * x - y) * f.lidNorm);
  if (f.arc > 0.001f) {
    // A thick, round-ended arc: the ^ of a happy squint.
    constexpr float kThickness = 5.5f;
    float arc;
    if (y <= f.arcEndY) {
      const float dy = y - f.arcCentre;
      arc = fabsf(sqrtf(x * x + dy * dy) - f.arcRadius) - kThickness;
    } else {
      const float end = fabsf(x) - f.arcEndX, dy = y - f.arcEndY;
      arc = sqrtf(end * end + dy * dy) - kThickness;
    }
    d = mixf(d, arc, f.arc);
  }
  return d;
}

bool within(int16_t v, int16_t lo, int16_t hi) { return v >= lo && v < hi; }

}  // namespace

Face::Rect Face::drawEyes(Display &display, uint32_t now, uint16_t background) {
  advance(now);
  const uint8_t mood = static_cast<uint8_t>(emotion_);
  const Rgb core = kColors[mood];
  const Rgb bright = mix(scale(core, 1.25f), kWhite, 0.15f);
  const Rgb deep = scale(core, 0.72f);
  const Rgb bg = from565(background);

  float blink = 0.0f;
  if (blinking_) {
    const float t = (now - blinkStartedAt_) / 170.0f;
    if (t >= 1.0f) {
      blinking_ = false;
      // Occasionally a quick double blink, which reads as alive.
      const bool twice = random() < 0.2f;
      nextBlinkAt_ = now + (twice ? 120 : 2200 + static_cast<uint32_t>(random() * 3200.0f));
    } else {
      blink = t < 0.4f ? t / 0.4f : (1.0f - t) / 0.6f;
    }
  }

  // Per-emotion body language on top of the gaze.
  const float seconds = now / 1000.0f;
  float bobX = 0.0f, bobY = 0.0f;
  switch (emotion_) {
    case Emotion::Excited: bobY = -fabsf(sinf(seconds * 7.5f)) * 7.0f; break;
    case Emotion::Happy: bobY = sinf(seconds * 5.0f) * 2.5f; break;
    case Emotion::Angry: bobX = sinf(seconds * 55.0f) * 1.2f; break;
    case Emotion::Sad: bobY = sinf(seconds * 1.6f) * 2.0f; break;
    case Emotion::Soft: bobY = sinf(seconds * 2.2f) * 1.5f; break;
    case Emotion::Sassy: bobX = -9.0f; break;  // side-eye
    case Emotion::Curious: bobX = 5.0f; bobY = -2.0f; break;
    default: break;
  }
  const float offsetX = gazeX_ + bobX, offsetY = gazeY_ + bobY;
  EyeFrame frames[2] = {
      frameFor(eyes_[0], kLeftEyeX - eyes_[0].dx + offsetX, kEyeY + eyes_[0].dy + offsetY, 1.0f, blink),
      frameFor(eyes_[1], kRightEyeX + eyes_[1].dx + offsetX, kEyeY + eyes_[1].dy + offsetY, -1.0f, blink),
  };
  const bool blush = extras_.blush > 0.01f;
  for (EyeFrame &f : frames) {
    if (blush) f.y1 = static_cast<int16_t>(fmaxf(f.y1, f.cy + f.hy + 24.0f));
    if (f.y0 < kEyesTop) f.y0 = kEyesTop;
    if (f.y1 > kEyesTop + kEyesHeight) f.y1 = kEyesTop + kEyesHeight;
    if (f.x0 < 0) f.x0 = 0;
    if (f.x1 > 240) f.x1 = 240;
  }

  // A tear forms under the outer corner of the right eye and falls.
  const EyeFrame &right = frames[1];
  const float tearCycle = fmodf(seconds, 2.2f) / 2.2f;
  const float tearX = right.cx + right.hx * 0.64f;
  const float tearY = right.cy + right.hy * 0.84f + tearCycle * tearCycle * 44.0f;
  const float tearAlpha = extras_.tear * sinf(tearCycle * 3.14159f);
  const bool tear = tearAlpha > 0.01f;
  const float sparkleSize = 9.0f + 3.0f * sinf(seconds * 9.0f);
  const float questionY = 116.0f + sinf(seconds * 3.0f) * 3.0f;
  const bool question = extras_.question > 0.01f;

  Rect drawn = unite(Rect{frames[0].x0, frames[0].y0, frames[0].x1, frames[0].y1},
                    Rect{frames[1].x0, frames[1].y0, frames[1].x1, frames[1].y1});
  if (tear) drawn = unite(drawn, around(tearX - 9.0f, tearY - 11.0f, tearX + 9.0f, tearY + 11.0f));
  if (question) drawn = unite(drawn, around(198.0f, questionY - 20.0f, 226.0f, questionY + 18.0f));
  drawn = clip(drawn, 0, kEyesTop, 240, kEyesHeight);
  // Nothing that affects a pixel changed since the last frame: skip it all.
  const float signature[] = {frames[0].cx, frames[0].cy, frames[0].hx, frames[0].hy, frames[0].lidTop,
                             frames[0].lidSlope, frames[0].arc, frames[1].cx, frames[1].cy, frames[1].hx,
                             frames[1].hy, frames[1].lidTop, frames[1].lidSlope, frames[1].arc,
                             extras_.blush, extras_.sparkle > 0.5f ? sparkleSize : 0.0f,
                             tear ? tearY : 0.0f, tearAlpha, question ? questionY : 0.0f,
                             extras_.question, static_cast<float>(mood)};
  static_assert(sizeof(signature) == sizeof(lastEyesSignature_), "signature size");
  bool unchanged = true;
  for (size_t i = 0; i < sizeof(signature) / sizeof(signature[0]); ++i) {
    // A quarter pixel of movement is invisible after anti-aliasing.
    if (fabsf(signature[i] - lastEyesSignature_[i]) > 0.02f) unchanged = false;
    lastEyesSignature_[i] = signature[i];
  }
  if (unchanged && drawn.x0 == lastEyes_.x0 && drawn.y0 == lastEyes_.y0) return Rect{0, 0, 0, 0};

  const Rect area = unite(drawn, lastEyes_);
  lastEyes_ = drawn;

  // The solid body of each eye is a vertical gradient, so its colour depends
  // only on the row: work it out once per row, not per pixel.
  uint16_t rowFill[2][kEyesHeight];
  for (int e = 0; e < 2; ++e) {
    for (int16_t row = 0; row < kEyesHeight; ++row) {
      const float ey = kEyesTop + row + 0.5f - frames[e].cy;
      const float t = clampf((ey + frames[e].hy) / (2.0f * frames[e].hy), 0.0f, 1.0f);
      rowFill[e][row] = to565(t < 0.5f ? mix(bright, core, t * 2.0f) : mix(core, deep, (t - 0.5f) * 2.0f));
    }
  }

  uint16_t row[240];
  for (int16_t py = area.y0; py < area.y1; ++py) {
    const float y = py + 0.5f;
    for (int16_t px = area.x0; px < area.x1; ++px) {
      uint16_t &out = row[px - area.x0];
      const EyeFrame *f = nullptr;
      for (const EyeFrame &candidate : frames) {
        if (within(px, candidate.x0, candidate.x1) && within(py, candidate.y0, candidate.y1)) f = &candidate;
      }
      const bool inTear = tear && fabsf(px - tearX) < 9.0f && fabsf(py - tearY) < 11.0f;
      const bool inQuestion = question && px >= 198 && px < 226 && py >= questionY - 20 && py < questionY + 18;
      if (!f && !inTear && !inQuestion) {
        out = background;
        continue;
      }
      const float x = px + 0.5f;
      Rgb c = bg;
      if (f) {
        const float ex = x - f->cx, ey = y - f->cy;
        const float d = sdEyeFast(*f, ex, ey);
        // Most pixels are plain background or solid eye; skip the blending.
        const bool blushNear = blush && fabsf(ey - f->hy - 14.0f) < 11.0f && fabsf(ex) < 22.0f;
        if (!inTear && !inQuestion && !blushNear) {
          if (d >= 12.0f) {
            out = background;
            continue;
          }
          const float shine = extras_.sparkle > 0.5f ? sparkleSize + 1.0f : f->shineR + 1.0f;
          const bool nearShine =
              f->shine > 0.01f &&
              ((fabsf(ex - f->shineX) < shine && fabsf(ey - f->shineY) < shine) ||
               (fabsf(ex - f->glintX) < f->glintR + 1.0f && fabsf(ey - f->glintY) < f->glintR + 1.0f));
          if (d <= -1.0f && !nearShine) {
            out = rowFill[f == &frames[0] ? 0 : 1][py - kEyesTop];
            continue;
          }
        }
        if (blush) {
          const float by = ey - f->hy - 14.0f;
          if (fabsf(by) < 11.0f && fabsf(ex) < 22.0f) {
            const float b = (length(ex / 18.0f, by / 7.5f) - 1.0f) * 7.5f;
            c = mix(c, kBlush, clampf(0.6f - b / 4.0f, 0.0f, 1.0f) * 0.85f * extras_.blush);
          }
        }
        if (d > 0.0f && d < 12.0f) {
          const float glow = 1.0f - d / 12.0f;
          c = mix(c, core, glow * glow * 0.32f);
        }
        const float body = cover(d);
        if (body > 0.0f) {
          const float t = clampf((ey + f->hy) / (2.0f * f->hy), 0.0f, 1.0f);
          Rgb fill = t < 0.5f ? mix(bright, core, t * 2.0f) : mix(core, deep, (t - 0.5f) * 2.0f);
          if (f->shine > 0.01f) {
            const float hx = ex - f->shineX, hy = ey - f->shineY;
            if (extras_.sparkle > 0.5f) {
              if (fabsf(hx) < sparkleSize + 1.0f && fabsf(hy) < sparkleSize + 1.0f) {
                fill = mix(fill, kWhite, cover(sdSparkle(hx, hy, sparkleSize)));
              }
            } else if (fabsf(hx) < f->shineR + 1.0f && fabsf(hy) < f->shineR + 1.0f) {
              fill = mix(fill, kWhite, cover(length(hx, hy) - f->shineR) * 0.95f);
            }
            const float gx = ex - f->glintX, gy = ey - f->glintY;
            if (fabsf(gx) < f->glintR + 1.0f && fabsf(gy) < f->glintR + 1.0f) {
              fill = mix(fill, kWhite, cover(length(gx, gy) - f->glintR) * 0.85f);
            }
          }
          c = mix(c, fill, body);
        }
      }
      if (inTear) {
        const float tx = x - tearX, ty = y - tearY;
        // An egg narrowing upward reads as a drop.
        const float squeeze = ty < 0.0f ? 1.0f + (-ty) * 0.09f : 1.0f;
        const float drop = (length(tx * squeeze / 4.5f, ty / 6.5f) - 1.0f) * 4.5f;
        c = mix(c, kTear, cover(drop) * tearAlpha);
      }
      if (inQuestion) {
        const float qx = x - 212.0f, qy = y - questionY;
        float ring = fabsf(length(qx, qy + 6.0f) - 7.0f) - 1.8f;
        if (qy > -6.0f && qx < 1.0f) ring = 99.0f;  // open the lower-left of the hook
        const float stem = sdSegment(qx, qy, 0.0f, 1.0f, 0.0f, 5.0f) - 1.8f;
        const float dot = length(qx, qy - 11.0f) - 2.2f;
        c = mix(c, core, cover(fminf(ring, fminf(stem, dot))) * extras_.question);
      }
      out = to565(c);
    }
    display.writeRow(area.x0, py, row, area.x1 - area.x0);
  }
  return area;
}

namespace {

constexpr int kStrokePoints = 17;

// The lip line of a drawn mouth at t in [-1, 1], in px below the mouth centre.
float strokeY(const Face::Mouth &mouth, float t) {
  switch (mouth.shape) {
    case Face::Mouth::kSmile:
    case Face::Mouth::kShy:
      return mouth.curve * ((1.0f - t * t) - 0.5f);
    case Face::Mouth::kSmirk: {
      // A lazy S: the left corner sags a touch, the right sweeps up.
      const float u = (t + 1.0f) * 0.5f;
      return 3.0f * (1.0f - u) * (1.0f - u) - mouth.asym * u * u * u + mouth.asym * 0.35f;
    }
    case Face::Mouth::kLens:
      break;
  }
  return 0.0f;
}

// Half thickness along the line: a smirk is a brush stroke, fat in the
// middle and flicking thin at the ends.
float strokeWidth(const Face::Mouth &mouth, float t) {
  constexpr float kLip = 3.2f;
  if (mouth.shape != Face::Mouth::kSmirk) return kLip;
  return 1.3f + 2.9f * (1.0f - t * t);
}

float sdEllipse(float x, float y, float rx, float ry) {
  return (length(x / rx, y / ry) - 1.0f) * fminf(rx, ry);
}

}  // namespace

// Neutral, soft and sassy: a stroked line with rounded ends. Speech opens it:
// neutral drops a cartoon "D" below the line, sassy opens lopsided on its
// raised side, and soft's shy smile melts into a small round "o".
Face::Rect Face::drawStrokeMouth(Display &display, float opening, uint16_t background) {
  const uint8_t mood = static_cast<uint8_t>(emotion_);
  const Rgb core = kColors[mood];
  const Rgb bg = from565(background);
  const Rgb inside = mix(bg, scale(core, 0.25f), 0.8f);
  const float half = mouth_.width * 0.5f;
  const bool smirk = mouth_.shape == Mouth::kSmirk;
  const bool shy = mouth_.shape == Mouth::kShy;

  float pointX[kStrokePoints], pointY[kStrokePoints], pointT[kStrokePoints];
  float top = 1e6f, bottom = -1e6f;
  for (int i = 0; i < kStrokePoints; ++i) {
    const float t = -1.0f + 2.0f * i / (kStrokePoints - 1);
    pointT[i] = t;
    pointX[i] = t * half;
    pointY[i] = strokeY(mouth_, t);
    top = fminf(top, pointY[i]);
    bottom = fmaxf(bottom, pointY[i]);
  }

  // The "D": between the lip line and a rounded bottom that sinks with speech.
  const float openX = smirk ? half * 0.35f : 0.0f;
  const float openRx = half * (smirk ? 0.55f : 0.72f);
  const float openDepth = mouth_.depth * opening;
  const bool dOpen = !shy && openDepth > 1.2f;
  // Soft's "o": grows from the middle of the smile as the line fades away.
  const float ring = shy ? clampf(opening * 1.6f, 0.0f, 1.0f) : 0.0f;
  const float ringRx = 6.0f + 7.0f * opening, ringRy = 5.0f + 8.0f * opening;
  const float ringY = strokeY(mouth_, 0.0f) + ringRy * 0.3f;
  const float lineAlpha = 1.0f - ring;

  bottom += (dOpen ? openDepth : 0.0f) + (shy ? ringRy * 1.4f : 0.0f) + 6.0f;
  top -= 6.0f + (shy ? ringRy : 0.0f);

  const Rect drawn = clip(around(kMouthX - half - 7.0f, kMouthY + top, kMouthX + half + 7.0f, kMouthY + bottom),
                         kMouthLeft, kMouthTop, kMouthWidth, kMouthHeight);
  const Rect area = unite(drawn, lastMouth_);
  lastMouth_ = drawn;

  for (int16_t px = area.x0; px < area.x1; ++px) {
    const float x = px + 0.5f - kMouthX;
    const bool near = fabsf(x) < half + 7.0f;
    for (int16_t py = area.y0; py < area.y1; ++py) {
      const float y = py + 0.5f - kMouthY;
      if (!near || y < top || y > bottom) {
        display.drawPixel(px, py, background);
        continue;
      }
      Rgb c = bg;
      if (dOpen) {
        const float u = (x - openX) / openRx;
        if (fabsf(u) < 1.1f) {
          const float lipY = strokeY(mouth_, clampf(x / half, -1.0f, 1.0f));
          const float round = sqrtf(fmaxf(0.0f, 1.0f - u * u));
          const float low = lipY + openDepth * round;
          const float edge = (fabsf(u) - 1.0f) * openRx;
          c = mix(c, inside, cover(fmaxf(fmaxf(lipY - y, y - low), edge)));
          c = mix(c, core, cover(fmaxf(fabsf(y - low) - 3.2f * (0.5f + 0.5f * round), edge)));
        }
      }
      if (lineAlpha > 0.01f) {
        float line = 1e6f;
        // Points are evenly spaced in x, so only the few segments near this
        // column can be closest.
        const int nearest = static_cast<int>((x / half + 1.0f) * 0.5f * (kStrokePoints - 1) + 0.5f);
        const int first = nearest - 2 < 1 ? 1 : nearest - 2;
        const int last = nearest + 3 > kStrokePoints ? kStrokePoints : nearest + 3;
        for (int i = first; i < last; ++i) {
          const float d = sdSegment(x, y, pointX[i - 1], pointY[i - 1], pointX[i], pointY[i]) -
                          strokeWidth(mouth_, (pointT[i - 1] + pointT[i]) * 0.5f);
          line = fminf(line, d);
        }
        c = mix(c, core, cover(line) * lineAlpha);
      }
      if (ring > 0.01f) {
        const float o = sdEllipse(x, y - ringY, ringRx, ringRy);
        Rgb fill = inside;
        if (mouth_.tongue > 0.01f) {
          const float tongue = sdEllipse(x, y - (ringY + ringRy * 0.75f), ringRx * 0.75f, ringRy * 0.45f);
          fill = mix(fill, kTongue, cover(tongue));
        }
        c = mix(c, fill, cover(o) * ring);
        c = mix(c, core, cover(fabsf(o) - 2.6f) * ring);
      }
      display.drawPixel(px, py, to565(c));
    }
  }
  return area;
}

Face::Rect Face::drawMouth(Display &display, float open, uint32_t now, uint16_t background) {
  const uint8_t mood = static_cast<uint8_t>(emotion_);
  tween(mouth_, kMouths[mood], approach(now - lastMouthAt_, 110.0f));
  mouth_.shape = kMouths[mood].shape;
  lastMouthAt_ = now;
  // A still, settled mouth (silence, no tween under way) needs no repaint.
  const float target = clampf(open, 0.0f, 1.0f);
  const Mouth &goal = kMouths[mood];
  const bool settled = fabsf(mouth_.width - goal.width) < 0.1f && fabsf(mouth_.curve - goal.curve) < 0.1f &&
                       fabsf(mouth_.depth - goal.depth) < 0.1f && fabsf(mouth_.asym - goal.asym) < 0.1f &&
                       fabsf(mouth_.rest - goal.rest) < 0.01f;
  const bool trembling = mouth_.curve < -9.0f && mouth_.shape == Mouth::kLens;
  if (settled && !trembling && fabsf(target - lastMouthOpening_) < 0.02f) return Rect{0, 0, 0, 0};
  lastMouthOpening_ = target;
  if (mouth_.shape != Mouth::kLens) {
    return drawStrokeMouth(display, clampf(mouth_.rest + mouth_.gain * clampf(open, 0.0f, 1.0f), 0.0f, 1.0f),
                           background);
  }

  const Rgb core = kColors[mood];
  const Rgb bg = from565(background);
  const Rgb inside = mix(bg, scale(core, 0.25f), 0.8f);
  const float opening = clampf(mouth_.rest + mouth_.gain * clampf(open, 0.0f, 1.0f), 0.0f, 1.0f);
  const float half = mouth_.width * 0.5f;
  const float seconds = now / 1000.0f;
  const bool ring = mouth_.ring > 0.5f;
  const float ringRadius = 5.0f + mouth_.depth * opening;

  Rect drawn;
  if (ring) {
    drawn = around(kMouthX - ringRadius - 5.0f, kMouthY - ringRadius - 5.0f,
                  kMouthX + ringRadius + 5.0f, kMouthY + ringRadius + 5.0f);
  } else {
    const float bend = fabsf(mouth_.curve) * 0.5f + fabsf(mouth_.asym) * 0.5f + 2.0f;
    drawn = around(kMouthX - half - 3.0f, kMouthY - bend - mouth_.depth * 0.3f - 5.0f,
                  kMouthX + half + 3.0f, kMouthY + bend + mouth_.depth + 5.0f);
  }
  drawn = clip(drawn, kMouthLeft, kMouthTop, kMouthWidth, kMouthHeight);
  const Rect area = unite(drawn, lastMouth_);
  lastMouth_ = drawn;

  for (int16_t px = area.x0; px < area.x1; ++px) {
    const float x = px + 0.5f - kMouthX;
    // Everything but the vertical test depends only on the column.
    const bool near = ring ? fabsf(x) < ringRadius + 4.0f : fabsf(x) < half + 2.0f;
    if (!near) {
      for (int16_t py = area.y0; py < area.y1; ++py) display.drawPixel(px, py, background);
      continue;
    }
    const float t = clampf(x / half, -1.0f, 1.0f);
    const float bow = 1.0f - t * t;
    // The lip line: bowed down at the centre for a smile, up for a frown,
    // tipped for a smirk, with a faint tremble when sad.
    float centre = mouth_.curve * (bow - 0.5f) - mouth_.asym * t * 0.5f;
    if (mouth_.curve < -9.0f) centre += sinf(x * 0.35f + seconds * 14.0f) * 0.8f * opening;
    const float gap = mouth_.depth * opening * sqrtf(bow);
    const float lip = 1.6f + 1.4f * bow;
    const float upperIn = centre - gap * 0.3f, lowerIn = centre + gap;
    const float cap = fabsf(x) - half;
    const float m = fmodf(x + 400.0f, 9.0f);
    const float slot = 0.8f - fminf(m, 9.0f - m);
    const float teethLine = upperIn + gap * 0.5f;

    for (int16_t py = area.y0; py < area.y1; ++py) {
      const float y = py + 0.5f - kMouthY;
      Rgb c = bg;
      if (ring) {
        const float r = length(x, y * 1.1f);
        c = mix(c, inside, cover(r - ringRadius));
        c = mix(c, core, cover(fabsf(r - ringRadius) - 2.4f));
      } else {
        const float outer = fmaxf(fmaxf(upperIn - lip - y, y - lowerIn - lip), cap);
        if (outer > 1.0f) {
          display.drawPixel(px, py, background);
          continue;
        }
        c = mix(c, core, cover(outer));
        if (gap > 0.8f) {
          const float inner = fmaxf(fmaxf(upperIn - y, y - lowerIn), cap + 3.0f);
          if (inner < 1.0f) {
            Rgb fill = inside;
            if (mouth_.tongue > 0.01f) {
              const float tongue = length(x / 1.5f, y - lowerIn) - gap * 0.55f;
              fill = mix(fill, kTongue, cover(tongue) * mouth_.tongue);
            }
            if (mouth_.teeth > 0.01f) {
              fill = mix(fill, kWhite, cover(fmaxf(y - teethLine, slot)) * mouth_.teeth);
            }
            c = mix(c, fill, cover(inner));
          }
        }
      }
      display.drawPixel(px, py, to565(c));
    }
  }
  return area;
}
