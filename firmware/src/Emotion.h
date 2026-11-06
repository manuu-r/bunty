#pragma once

#include <stdint.h>
#include <string.h>

// The feeling the gateway tags each spoken sentence with. The order matches
// cfg::dance::kStyles.
enum class Emotion : uint8_t {
  Neutral, Soft, Happy, Excited, Angry, Sad, Curious, Sassy, Count,
};

inline Emotion parseEmotion(const char *name) {
  static const char *const kNames[] = {
      "neutral", "soft", "happy", "excited", "angry", "sad", "curious", "sassy",
  };
  for (uint8_t i = 0; i < static_cast<uint8_t>(Emotion::Count); ++i) {
    if (strcmp(name, kNames[i]) == 0) return static_cast<Emotion>(i);
  }
  return Emotion::Neutral;
}

inline const char *emotionName(Emotion emotion) {
  static const char *const kNames[] = {
      "neutral", "soft", "happy", "excited", "angry", "sad", "curious", "sassy",
  };
  const uint8_t index = static_cast<uint8_t>(emotion);
  return index < static_cast<uint8_t>(Emotion::Count) ? kNames[index] : "neutral";
}
