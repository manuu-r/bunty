#pragma once

#include "Runtime.h"

#include "Config.h"
#include "Emotion.h"
#include "ServoMotion.h"
#include "Voice.h"
#include "SpeechGesture.h"

// Bunty's head during a conversation. It cocks its head while listening,
// glances aside while thinking, and while speaking performs the gesture
// pattern of the current sentence's emotion. Back home when it ends.
class Dance {
 public:
  void attach(ServoMotion *turn, uint8_t turnHome, ServoMotion *tilt,
              uint8_t tiltHome);
  // Call from loop(); paces itself. The servos are Dance's while active().
  void service(const Voice &voice, uint32_t now);
  bool active() const { return claimed_; }
  // Holds the head turned this many degrees (right positive) for the coming
  // conversation, under every pose it makes. Cleared when the head homes.
  void setBearing(float yawDegrees, uint32_t now) {
    bearing_ = yawDegrees;
    bearingAt_ = now;
  }

 private:
  void aim(float turnOffset, float tiltOffset, float degreesPerSecond);

  ServoMotion *turn_ = nullptr;
  ServoMotion *tilt_ = nullptr;
  float turnHome_ = 90.0f;
  float tiltHome_ = 90.0f;
  std::atomic<float> bearing_{0.0f};
  std::atomic<uint32_t> bearingAt_{0};
  bool claimed_ = false;
  bool homing_ = false;
  bool gesturing_ = false,visualGesture_=false;
  uint32_t visualGeneration_=0;
  uint32_t gestureSince_ = 0;
  speechGesture::Request gesture_;
  uint32_t lastTickAt_ = 0;
  uint32_t homingSince_ = 0;

  cfg::dance::Style style_ = cfg::dance::kStyles[0];
  float beat_ = 0.0f;
  float yawPhase_ = 0.0f;
  float tiltPhase_ = 0.0f;
};
