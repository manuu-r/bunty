#pragma once

#include <Arduino.h>

#include "ServoMotion.h"
#include "Voice.h"

// Bunty's head during a conversation. It cocks its head while listening,
// glances aside while thinking, and while speaking the tilt servo nods on each
// syllable onset as the yaw servo sweeps through phrases, both scaled by the
// loudness actually reaching the speaker. Back home when the conversation ends.
class Dance {
 public:
  void attach(ServoMotion *turn, uint8_t turnHome, ServoMotion *tilt,
              uint8_t tiltHome);
  // Call from loop(); paces itself. The servos are Dance's while active().
  void service(Voice::Phase phase, float level, uint32_t now);
  bool active() const { return claimed_; }

 private:
  void aim(float turnOffset, float tiltOffset, float degreesPerSecond);

  ServoMotion *turn_ = nullptr;
  ServoMotion *tilt_ = nullptr;
  float turnHome_ = 90.0f;
  float tiltHome_ = 90.0f;
  bool claimed_ = false;
  bool homing_ = false;
  uint32_t lastTickAt_ = 0;
  uint32_t homingSince_ = 0;

  float fast_ = 0.0f;
  float slow_ = 0.0f;
  float sweepPhase_ = 0.0f;
  float accent_ = 0.0f;
  float accentSign_ = 1.0f;
  bool onsetArmed_ = true;
  uint32_t lastAccentAt_ = 0;
};
