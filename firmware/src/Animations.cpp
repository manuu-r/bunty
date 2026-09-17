#include "Animations.h"

#include <math.h>

#include "Config.h"
#include "Fonts.h"
#include "flow32/graphics/Display.h"

namespace {

constexpr int16_t kScreenWidth = cfg::ui::kScreenWidth;
constexpr int16_t kScreenHeight = cfg::ui::kScreenHeight;
constexpr int16_t kContentTop = cfg::ui::kStatusBarHeight;
constexpr int16_t kContentHeight = kScreenHeight - kContentTop;
constexpr int16_t kSpeakingLeft = 22;
constexpr int16_t kSpeakingTop = 97;
constexpr int16_t kSpeakingWidth = 196;
constexpr int16_t kSpeakingHeight = 142;
constexpr uint32_t kSpeakingFrameTimeMs = 105;
// The eyes end at y=239; the mouth band starts below that and stays clear of
// the 320 px bottom edge at full opening.
constexpr int16_t kMouthCenterX = 120;
constexpr int16_t kMouthCenterY = 274;
constexpr int16_t kMouthHalfWidth = 38;
constexpr int16_t kMouthMinGap = 2;
constexpr int16_t kMouthMaxGap = 20;
constexpr int16_t kMouthStroke = 3;
constexpr int16_t kMouthMargin = 4;
constexpr int16_t kMouthBandTop =
    kMouthCenterY - kMouthMaxGap - kMouthStroke - kMouthMargin;
constexpr int16_t kMouthBandHeight =
    (kMouthMaxGap + kMouthStroke + kMouthMargin) * 2;
constexpr int16_t kMouthBandLeft = kMouthCenterX - kMouthHalfWidth - kMouthMargin;
constexpr int16_t kMouthBandWidth = (kMouthHalfWidth + kMouthMargin) * 2;
constexpr int16_t kMouthSegments = 16;

// Listening glyph: sits in the gap between the 32 px status bar and the eyes,
// which start at y=97.
constexpr int16_t kWakeBandTop = 40;
constexpr int16_t kWakeBandHeight = 54;
constexpr int16_t kWakeCenterX = 120;
constexpr int16_t kWakeCenterY = 58;
constexpr int16_t kWakeLabelTop = 78;

// Scrolling capture level history along the bottom edge.
constexpr int16_t kWaveBandTop = 288;
constexpr int16_t kWaveBandHeight = 30;
constexpr int16_t kWaveMidline = kWaveBandTop + kWaveBandHeight / 2;
constexpr int16_t kWaveBarWidth = 3;
constexpr int16_t kWaveBarGap = 1;
constexpr uint32_t kSleepFrameTimeMs = 650;
constexpr uint32_t kWakeFrameTimeMs = 110;

using cfg::rgb565;

constexpr uint16_t kBackground = cfg::ui::kBackground;
constexpr uint16_t kEyeCyan = rgb565(17, 236, 232);
constexpr uint16_t kEyeGlow = rgb565(5, 62, 72);
constexpr uint16_t kSleepCyan = rgb565(25, 164, 174);
constexpr uint16_t kSleepGlow = rgb565(9, 37, 49);

enum class RoboEyeShape : uint8_t {
  Pill,
  Focused,
  Squint,
  SoftSquare,
  Dot,
  Sleeping,
};

int16_t smaller(int16_t left, int16_t right) {
  return left < right ? left : right;
}

// Flow32's Display provides anti-aliased rounded rectangles on its RGB565
// framebuffer. Masking a few corners turns that one primitive into the same
// solid, geometric eye vocabulary as the reference: pills, angled focus eyes,
// squints, dots, and concave sleeping lids.
void fillRoboEyeShape(Display &display, int16_t centerX, int16_t centerY,
                      int16_t width, int16_t height, bool leftEye,
                      RoboEyeShape shape, uint16_t color) {
  if (width < 2 || height < 2) return;
  const int16_t x = centerX - width / 2;
  const int16_t y = centerY - height / 2;

  if (shape == RoboEyeShape::Dot) {
    display.fillCircle(centerX, centerY, smaller(width, height) / 2, color);
    return;
  }

  int16_t radius = smaller(width, height) / 3;
  if (shape == RoboEyeShape::Squint) radius = height / 2;
  if (shape == RoboEyeShape::SoftSquare) radius = smaller(radius, 10);
  if (shape == RoboEyeShape::Sleeping) radius = smaller(radius, 12);
  if (radius < 2) radius = 2;
  display.fillRoundRect(x, y, width, height, radius, color);

  if (shape == RoboEyeShape::Focused) {
    const int16_t cut = smaller(18, height / 3);
    if (leftEye) {
      display.fillTriangle(centerX - 2, y - 2, x + width + 3, y - 2,
                           x + width + 3, y + cut, kBackground);
    } else {
      display.fillTriangle(x - 3, y - 2, centerX + 2, y - 2, x - 3,
                           y + cut, kBackground);
    }
  } else if (shape == RoboEyeShape::Sleeping) {
    // Carve a soft concave lower edge, leaving a chunky closed eyelid cap.
    const int16_t notchRadius = smaller(width / 2 - 3, height / 2 + 5);
    display.fillCircle(centerX, y + height + 5, notchRadius, kBackground);
  }
}

void drawRoboEye(Display &display, int16_t centerX, int16_t centerY,
                 int16_t width, int16_t height, bool leftEye,
                 RoboEyeShape shape, bool sleeping = false) {
  const uint16_t glow = sleeping ? kSleepGlow : kEyeGlow;
  const uint16_t core = sleeping ? kSleepCyan : kEyeCyan;
  fillRoboEyeShape(display, centerX, centerY, width + 10, height + 10,
                   leftEye, shape, glow);
  fillRoboEyeShape(display, centerX, centerY, width, height, leftEye, shape,
                   core);
}

void drawText(Display &display, const GFXfont *font, const char *text,
              int16_t x, int16_t y, uint16_t color) {
  display.setFont(font);
  display.setTextSize(1);
  display.setTextColor(color);
  display.setTextWrap(false);
  display.setCursor(x, y);
  display.print(text);
}

uint8_t advanceFrame(uint32_t now, uint32_t &lastFrameAt,
                     uint32_t frameTimeMs, uint8_t frame,
                     uint8_t frameCount) {
  const uint32_t elapsed = now - lastFrameAt;
  const uint32_t steps = elapsed / frameTimeMs;
  lastFrameAt += steps * frameTimeMs;
  return static_cast<uint8_t>((frame + steps) % frameCount);
}

}  // namespace

void BuntyAnimations::beginSpeaking(uint32_t now, bool present) {
  frame_ = 0;
  animationStartedAt_ = now;
  lastFrameAt_ = now;
  display_.fillRect(0, kContentTop, kScreenWidth, kContentHeight, kBackground);
  drawSpeakingFrame();
  if (present) {
    display_.present(0, kContentTop, kScreenWidth, kContentHeight);
  }
}

void BuntyAnimations::serviceSpeaking(uint32_t now) {
  if (now - lastFrameAt_ < kSpeakingFrameTimeMs) return;
  frame_ = advanceFrame(now, lastFrameAt_, kSpeakingFrameTimeMs, frame_, 12);
  drawSpeakingFrame();
  display_.present(kSpeakingLeft, kSpeakingTop, kSpeakingWidth,
                   kSpeakingHeight);
}

void BuntyAnimations::drawSpeakingFrame() {
  struct EyePose {
    RoboEyeShape leftShape;
    RoboEyeShape rightShape;
    int8_t leftY;
    int8_t rightY;
    uint8_t leftWidth;
    uint8_t leftHeight;
    uint8_t rightWidth;
    uint8_t rightHeight;
  };
  static constexpr EyePose kPoses[12] = {
      {RoboEyeShape::Pill, RoboEyeShape::Pill, 0, 0, 46, 62, 46, 62},
      {RoboEyeShape::Focused, RoboEyeShape::Focused, -1, -1, 50, 66, 50,
       66},
      {RoboEyeShape::Pill, RoboEyeShape::Pill, -3, 2, 48, 70, 46, 56},
      {RoboEyeShape::Squint, RoboEyeShape::Squint, 1, 1, 58, 22, 58, 22},
      {RoboEyeShape::Pill, RoboEyeShape::SoftSquare, -3, 4, 44, 64, 48, 46},
      {RoboEyeShape::SoftSquare, RoboEyeShape::Dot, 2, 2, 48, 48, 44, 44},
      {RoboEyeShape::Focused, RoboEyeShape::Focused, 0, 0, 52, 68, 52, 68},
      {RoboEyeShape::Pill, RoboEyeShape::Squint, -3, 7, 46, 64, 58, 20},
      {RoboEyeShape::Squint, RoboEyeShape::Pill, 7, -3, 58, 20, 46, 64},
      {RoboEyeShape::Pill, RoboEyeShape::Pill, 1, 1, 52, 54, 52, 54},
      {RoboEyeShape::Squint, RoboEyeShape::Squint, 3, 3, 60, 16, 60, 16},
      {RoboEyeShape::Pill, RoboEyeShape::Pill, 0, 0, 48, 64, 48, 64},
  };

  const EyePose &pose = kPoses[frame_ % 12];
  display_.fillRect(kSpeakingLeft, kSpeakingTop, kSpeakingWidth,
                    kSpeakingHeight, kBackground);

  constexpr int16_t kLeftEyeX = 72;
  constexpr int16_t kRightEyeX = 168;
  constexpr int16_t kEyeY = 167;

  // Solid cyan shapes are the entire speaking character. The pair moves
  // through the reference library's pill, focus, squint, and asymmetric
  // silhouettes without pupils, a mouth, a body, or a greeting label.
  drawRoboEye(display_, kLeftEyeX, kEyeY + pose.leftY, pose.leftWidth,
              pose.leftHeight, true, pose.leftShape);
  drawRoboEye(display_, kRightEyeX, kEyeY + pose.rightY, pose.rightWidth,
              pose.rightHeight, false, pose.rightShape);
}

void BuntyAnimations::beginSleeping(uint32_t now) {
  frame_ = 0;
  animationStartedAt_ = now;
  lastFrameAt_ = now;
  drawSleepFrame();
}

void BuntyAnimations::serviceSleeping(uint32_t now) {
  if (now - lastFrameAt_ < kSleepFrameTimeMs) return;
  frame_ = advanceFrame(now, lastFrameAt_, kSleepFrameTimeMs, frame_, 8);
  drawSleepFrame();
}

void BuntyAnimations::drawSleepFrame() {
  static constexpr int8_t kBreath[8] = {0, -1, -2, -1, 0, 1, 2, 1};
  const uint8_t phase = frame_ % 8;
  const int16_t bob = kBreath[phase];
  display_.fillRect(0, kContentTop, kScreenWidth, kContentHeight, kBackground);

  // Sleep uses the reference's concave closed-eye caps plus a quiet ZZZ.
  const int16_t eyeY = 170 + bob;
  const int16_t breathWidth = 48 + (phase == 2 || phase == 6 ? 2 : 0);
  drawRoboEye(display_, 72, eyeY, breathWidth, 34, true,
              RoboEyeShape::Sleeping, true);
  drawRoboEye(display_, 168, eyeY, breathWidth, 34, false,
              RoboEyeShape::Sleeping, true);

  const int16_t drift = static_cast<int16_t>(phase / 2);
  drawText(display_, BuntyFonts::kBody, "z", 157, 133 - drift, kSleepGlow);
  drawText(display_, BuntyFonts::kBody, "Z", 178, 109 - drift, kSleepCyan);
  drawText(display_, BuntyFonts::kHeading, "Z", 198, 80 - drift,
           kSleepCyan);
  display_.present(0, kContentTop, kScreenWidth, kContentHeight);
}

void BuntyAnimations::beginWaking(uint32_t now) {
  frame_ = 0;
  animationStartedAt_ = now;
  lastFrameAt_ = now;
  drawWakeFrame();
}

bool BuntyAnimations::serviceWaking(uint32_t now) {
  if (now - animationStartedAt_ >= wakeDurationMs()) return false;
  if (now - lastFrameAt_ >= kWakeFrameTimeMs) {
    frame_ = advanceFrame(now, lastFrameAt_, kWakeFrameTimeMs, frame_, 15);
    drawWakeFrame();
  }
  return true;
}

void BuntyAnimations::drawWakeFrame() {
  static constexpr uint8_t kLeftHeights[15] = {
      8, 8, 12, 20, 32, 46, 60, 70, 64, 68, 64, 62, 64, 64, 64};
  static constexpr uint8_t kRightHeights[15] = {
      8, 8, 8, 14, 26, 40, 56, 68, 62, 70, 66, 62, 64, 64, 64};
  const uint8_t phase = frame_ % 15;
  const int16_t leftHeight = kLeftHeights[phase];
  const int16_t rightHeight = kRightHeights[phase];
  display_.fillRect(0, kContentTop, kScreenWidth, kContentHeight, kBackground);

  constexpr int16_t kLeftEyeX = 72;
  constexpr int16_t kRightEyeX = 168;
  constexpr int16_t kEyeY = 167;
  const RoboEyeShape leftShape = leftHeight <= 20 ? RoboEyeShape::Squint
                                                   : RoboEyeShape::Pill;
  const RoboEyeShape rightShape = rightHeight <= 20 ? RoboEyeShape::Squint
                                                     : RoboEyeShape::Pill;
  drawRoboEye(display_, kLeftEyeX, kEyeY, leftHeight <= 20 ? 58 : 48,
              leftHeight, true, leftShape);
  drawRoboEye(display_, kRightEyeX, kEyeY, rightHeight <= 20 ? 58 : 48,
              rightHeight, false, rightShape);
  display_.present(0, kContentTop, kScreenWidth, kContentHeight);
}

void BuntyAnimations::drawSpeakingMouth(uint8_t openness) {
  if (openness > 100) openness = 100;

  // Repaint only the mouth band so the eyes above keep their own frame timing.
  display_.fillRect(kMouthBandLeft, kMouthBandTop, kMouthBandWidth,
                    kMouthBandHeight, kBackground);

  const int16_t gap =
      kMouthMinGap +
      static_cast<int16_t>((kMouthMaxGap - kMouthMinGap) * openness / 100);

  // Two parabolic arcs meeting at the corners: a lens that widens with
  // loudness. Drawn as short stroked segments to stay in the same geometric
  // line vocabulary as the eyes rather than becoming a filled blob.
  int16_t previousX = kMouthCenterX - kMouthHalfWidth;
  int16_t previousUpper = kMouthCenterY;
  int16_t previousLower = kMouthCenterY;
  for (int16_t segment = 1; segment <= kMouthSegments; ++segment) {
    const int16_t x = kMouthCenterX - kMouthHalfWidth +
                      (2 * kMouthHalfWidth * segment) / kMouthSegments;
    // Normalised to -100..100 across the mouth; the parabola is 0 at the
    // corners and full height at the centre.
    const int32_t offsetFromCentre =
        (static_cast<int32_t>(x - kMouthCenterX) * 100) / kMouthHalfWidth;
    const int32_t bow =
        (100 * 100 - offsetFromCentre * offsetFromCentre) / 100;
    const int16_t spread = static_cast<int16_t>(gap * bow / 100);
    const int16_t upper = kMouthCenterY - spread;
    const int16_t lower = kMouthCenterY + spread;
    for (int16_t stroke = 0; stroke < kMouthStroke; ++stroke) {
      display_.drawLine(previousX, previousUpper + stroke, x, upper + stroke,
                        kEyeCyan);
      display_.drawLine(previousX, previousLower - stroke, x, lower - stroke,
                        kEyeCyan);
    }
    previousX = x;
    previousUpper = upper;
    previousLower = lower;
  }

  display_.present(kMouthBandLeft, kMouthBandTop, kMouthBandWidth,
                   kMouthBandHeight);
}

void BuntyAnimations::drawWakeIcon() {
  display_.fillRect(0, kWakeBandTop, kScreenWidth, kWakeBandHeight,
                    kBackground);

  // A microphone capsule with a cradle, stem, and base, in the same chunky
  // cyan vocabulary as the eyes.
  constexpr int16_t kCapsuleWidth = 12;
  constexpr int16_t kCapsuleHeight = 18;
  display_.fillRoundRect(kWakeCenterX - kCapsuleWidth / 2,
                         kWakeCenterY - kCapsuleHeight / 2 - 4, kCapsuleWidth,
                         kCapsuleHeight, kCapsuleWidth / 2, kEyeCyan);
  for (int16_t thickness = 0; thickness < 2; ++thickness) {
    display_.drawCircle(kWakeCenterX, kWakeCenterY - 5 + thickness, 10,
                        kEyeCyan);
  }
  // Mask the upper half of the cradle so it reads as a cup, not a ring.
  display_.fillRect(kWakeCenterX - 11, kWakeCenterY - 16, 22, 11, kBackground);
  display_.fillRect(kWakeCenterX - 1, kWakeCenterY + 5, 3, 5, kEyeCyan);
  display_.fillRect(kWakeCenterX - 7, kWakeCenterY + 10, 15, 3, kEyeCyan);

  // Sound arcs either side, dimmer so the capsule stays the focal point.
  for (int16_t side = -1; side <= 1; side += 2) {
    for (int16_t ring = 1; ring <= 2; ++ring) {
      const int16_t radius = 7 + ring * 6;
      const int16_t x = kWakeCenterX + side * (kCapsuleWidth / 2 + 5);
      for (int16_t dy = -radius / 2; dy <= radius / 2; ++dy) {
        const int16_t dx = static_cast<int16_t>(
            sqrtf(static_cast<float>(radius * radius - dy * dy)) / 2.0f);
        display_.drawPixel(x + side * dx, kWakeCenterY - 5 + dy, kEyeGlow);
      }
    }
  }

  // "LISTENING" under the glyph, centred the same way the dashboard centres
  // its own copy.
  display_.setFont(BuntyFonts::kBody);
  display_.setTextSize(1);
  display_.setTextColor(kEyeCyan);
  display_.setTextWrap(false);
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  display_.getTextBounds("LISTENING", 0, 0, &x1, &y1, &width, &height);
  display_.setCursor((kScreenWidth - static_cast<int16_t>(width)) / 2 - x1,
                     kWakeLabelTop - y1);
  display_.print("LISTENING");

  display_.present(0, kWakeBandTop, kScreenWidth, kWakeBandHeight);
}

void BuntyAnimations::clearWakeIcon() {
  display_.fillRect(0, kWakeBandTop, kScreenWidth, kWakeBandHeight,
                    kBackground);
  display_.present(0, kWakeBandTop, kScreenWidth, kWakeBandHeight);
}

void BuntyAnimations::drawInputWaveform(const uint8_t *levels, size_t count,
                                        size_t head) {
  if (!levels || count == 0) return;
  display_.fillRect(0, kWaveBandTop, kScreenWidth, kWaveBandHeight,
                    kBackground);

  // A faint centre line so a silent stretch still reads as "capturing".
  display_.drawFastHLine(4, kWaveMidline, kScreenWidth - 8, kEyeGlow);

  const int16_t stride = kWaveBarWidth + kWaveBarGap;
  const int16_t maxBars = (kScreenWidth - 8) / stride;
  const int16_t bars =
      static_cast<int16_t>(count) < maxBars ? static_cast<int16_t>(count) : maxBars;
  const int16_t half = kWaveBandHeight / 2 - 2;

  for (int16_t i = 0; i < bars; ++i) {
    // Oldest bar leftmost: walk back from the newest sample.
    const size_t index = (head + count - static_cast<size_t>(bars - i)) % count;
    int16_t amplitude = static_cast<int16_t>(levels[index]) * half / 100;
    if (amplitude < 1) amplitude = 1;
    const int16_t x = 4 + i * stride;
    display_.fillRect(x, kWaveMidline - amplitude, kWaveBarWidth,
                      amplitude * 2, kEyeCyan);
  }

  display_.present(0, kWaveBandTop, kScreenWidth, kWaveBandHeight);
}

void BuntyAnimations::clearInputWaveform() {
  display_.fillRect(0, kWaveBandTop, kScreenWidth, kWaveBandHeight,
                    kBackground);
  display_.present(0, kWaveBandTop, kScreenWidth, kWaveBandHeight);
}
