#pragma once
#include <stdint.h>
#include "Emotion.h"
class Display;

// Matte, anti-aliased eyes and a compact speech mouth. Fixed row scratch,
// dirty-band presentation, and held poses keep rendering bounded on ESP32.
class Face {
 public:
  static constexpr int16_t kEyesTop=96,kEyesHeight=122;
  static constexpr int16_t kMouthLeft=64,kMouthTop=220,kMouthWidth=112,kMouthHeight=38;
  struct Rect {int16_t x0,y0,x1,y1;};
  enum class Attention : uint8_t {Idle,Listening,Thinking,Working,Speaking};
  void reset(uint32_t now);
  void setEmotion(Emotion emotion) {emotion_=uint8_t(emotion)<uint8_t(Emotion::Count)?emotion:Emotion::Neutral;}
  void setAttention(Attention attention) {attention_=attention;}
  Rect drawEyes(Display &,uint32_t now,uint16_t background);
  Rect drawMouth(Display &,float open,uint32_t now,uint16_t background,float opacity=1.f);
  void invalidateMouth(){lastMouthOpen_=-1;lastMouthMood_=255;}
  struct Eye {float width,height,round,dy,lid,slant,arc;};
  struct Mouth {float width,curve,depth,asym;};
 private:
  void advance(uint32_t now);
  float random();
  Emotion emotion_=Emotion::Neutral;
  Attention attention_=Attention::Idle;
  Eye eyes_[2]{};
  Mouth mouth_{};
  float gazeX_=0,gazeY_=0,targetX_=0,targetY_=0;
  float lastSignature_[19]{};
  float mouthOpen_=0,lastMouthOpen_=-1,lastMouthAlpha_=1;
  uint8_t lastMouthMood_=255;
  Rect lastEyes_{},lastMouth_{};
  uint32_t lastAdvanceAt_=0,lastMouthAt_=0,nextBlinkAt_=0,blinkAt_=0,nextGlanceAt_=0;
  uint32_t seed_=0x9e3779b9u;
  bool blinking_=false;
};
