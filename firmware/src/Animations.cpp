#include "Animations.h"

#include <math.h>
#include <initializer_list>

#include "Config.h"
#include "Fonts.h"
#include "VoiceScreen.h"
#include "flow32/graphics/Display.h"

namespace {

constexpr int16_t kScreenWidth = cfg::ui::kScreenWidth;
constexpr int16_t kScreenHeight = cfg::ui::kScreenHeight;
constexpr int16_t kContentTop = cfg::ui::kStatusBarHeight;
constexpr int16_t kContentHeight = kScreenHeight - kContentTop;
constexpr uint32_t kSpeakingFrameTimeMs = 80;

constexpr int16_t kWakeBandTop=32,kWakeBandHeight=64;
constexpr uint32_t kSleepFrameTimeMs = 650;
constexpr uint32_t kWakeFrameTimeMs = 110;

using cfg::rgb565;

constexpr uint16_t kBackground = cfg::ui::kBackground;
constexpr uint16_t kFaceAccent = rgb565(193, 225, 156);
constexpr uint16_t kSleepInk = rgb565(111, 129, 100);

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
  face_.setEmotion(emotion_);
  face_.reset(now);
  display_.fillRect(0, kContentTop, kScreenWidth, kContentHeight, kBackground);
  drawSpeakingFrame();
  face_.drawMouth(display_, 0.0f, now, kBackground);
  if (present) {
    display_.present(0, kContentTop, kScreenWidth, kContentHeight);
  }
}

void BuntyAnimations::serviceSpeaking(uint32_t now) {
  if (now - lastFrameAt_ < kSpeakingFrameTimeMs) return;
  lastFrameAt_ = now;
  face_.setEmotion(emotion_);
  const Face::Rect area = face_.drawEyes(display_, now, kBackground);
  if (area.x1 > area.x0) display_.present(area.x0, area.y0, area.x1 - area.x0, area.y1 - area.y0);
}

void BuntyAnimations::drawSpeakingFrame() {
  face_.setEmotion(emotion_);
  face_.drawEyes(display_, millis(), kBackground);
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
  display_.fillRect(0,kContentTop,kScreenWidth,kContentHeight,kBackground);
  const int width=52+(frame_%8==2?2:0);
  for(const int center:{72,168})
    display_.fillRoundRect(center-width/2,157,width,6,3,kSleepInk);
  display_.fillRoundRect(108,232,24,3,1,kSleepInk);
  display_.present(0,kContentTop,kScreenWidth,kContentHeight);
}

void BuntyAnimations::beginWaking(uint32_t now) {
  face_.setAttention(Face::Attention::Idle);
  face_.setEmotion(Emotion::Neutral);
  face_.reset(now);
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
  static constexpr uint8_t heights[15]={6,6,9,16,28,43,60,72,80,78,76,76,76,76,76};
  display_.fillRect(0,kContentTop,kScreenWidth,kContentHeight,kBackground);
  if(frame_>=11) {
    face_.reset(millis());
    face_.drawEyes(display_,millis(),kBackground);
    face_.drawMouth(display_,0,millis(),kBackground);
  } else {
    const int height=heights[frame_%15];
    for(const int center:{72,168})
      display_.fillRoundRect(center-29,156-height/2,58,height,height<24?height/2:20,
        rgb565(233,241,223));
    display_.fillRoundRect(106,232,28,3,1,kFaceAccent);
  }
  display_.present(0,kContentTop,kScreenWidth,kContentHeight);
}

void BuntyAnimations::drawSpeakingMouth(uint8_t openness,uint8_t opacity) {
  face_.setEmotion(emotion_);
  const Face::Rect area =
      face_.drawMouth(display_, (openness > 100 ? 100 : openness) / 100.0f, millis(), kBackground,opacity/255.f);
  if (area.x1 > area.x0) display_.present(area.x0, area.y0, area.x1 - area.x0, area.y1 - area.y0);
}

void BuntyAnimations::drawWakeIcon(const char *label) {
  voiceScreen::heading(display_,label);
  display_.present(0,32,kScreenWidth,64);
}

void BuntyAnimations::clearWakeIcon() {
  display_.fillRect(0, kWakeBandTop, kScreenWidth, kWakeBandHeight,
                    kBackground);
  display_.present(0, kWakeBandTop, kScreenWidth, kWakeBandHeight);
}

void BuntyAnimations::drawReaction(const char *text) {
  if (!text || !text[0]) return;
  constexpr int16_t kReactionTop = 232;
  constexpr int16_t kReactionHeight = 68;
  display_.fillRect(0, kReactionTop, kScreenWidth, kReactionHeight, kBackground);

  const size_t len = strlen(text);
  const GFXfont *font = (len <= 12) ? BuntyFonts::kHeading : BuntyFonts::kBody;
  display_.setFont(font);
  display_.setTextSize(1);
  display_.setTextWrap(false);
  display_.setTextColor(cfg::ui::kText);

  int16_t x0, y0;
  uint16_t w, h;
  display_.getTextBounds(text, 0, 0, &x0, &y0, &w, &h);

  const int16_t x = (kScreenWidth - int16_t(w)) / 2 - x0;
  const int16_t y = kReactionTop + (kReactionHeight + int16_t(h)) / 2;
  display_.setCursor(x, y);
  display_.print(text);

  display_.present(0, kReactionTop, kScreenWidth, kReactionHeight);
}

void BuntyAnimations::clearReaction() {
  constexpr int16_t kReactionTop = 232;
  constexpr int16_t kReactionHeight = 68;
  display_.fillRect(0, kReactionTop, kScreenWidth, kReactionHeight, kBackground);
  face_.invalidateMouth();
  face_.drawMouth(display_, 0.0f, millis(), kBackground);
  display_.present(0, kReactionTop, kScreenWidth, kReactionHeight);
}

void BuntyAnimations::drawProgress(const char *status, int percent, uint32_t now) {
  display_.fillRect(0, kWakeBandTop, kScreenWidth, kWakeBandHeight, kBackground);
  if (status && status[0]) {
    display_.setFont(BuntyFonts::kBody);
    display_.setTextSize(1);
    display_.setTextWrap(false);
    display_.setTextColor(cfg::ui::kText);
    int16_t x0, y0;
    uint16_t w, h;
    display_.getTextBounds(status, 0, 0, &x0, &y0, &w, &h);
    const int16_t x = (kScreenWidth - int16_t(w)) / 2 - x0;
    display_.setCursor(x, kWakeBandTop + 24);
    display_.print(status);
  }
  constexpr int16_t barX = 40, barY = kWakeBandTop + 36, barW = 160, barH = 5;
  display_.fillRoundRect(barX, barY, barW, barH, 2, cfg::ui::kBorder);
  if (percent >= 0 && percent <= 100) {
    const int16_t fillW = (barW * percent) / 100;
    if (fillW > 0) {
      display_.fillRoundRect(barX, barY, fillW, barH, 2, cfg::ui::kAccent);
    }
  } else {
    constexpr int16_t pillW = 36;
    const int16_t travel = barW - pillW;
    const float cycle = sinf(float(now % 2000) * (2.0f * 3.14159f / 2000.0f));
    const int16_t pillX = barX + int16_t((cycle * 0.5f + 0.5f) * float(travel));
    display_.fillRoundRect(pillX, barY, pillW, barH, 2, cfg::ui::kAccent);
  }
  display_.present(0, kWakeBandTop, kScreenWidth, kWakeBandHeight);
}

void BuntyAnimations::clearProgress() {
  clearWakeIcon();
}



