#pragma once
#include <cstdint>
namespace voiceScreen {
// A short eased handoff keeps the same face alive; only small bands repaint.
inline float transition(uint32_t now,uint32_t started) {
  const uint32_t elapsed=now-started;
  const float t=elapsed>=320?1.f:float(elapsed)/320.f;
  return t*t*(3.f-2.f*t);
}
inline uint8_t opacity(float value) {
  return value<=0?0:value>=1?255:uint8_t(value*255.f+.5f);
}
}
