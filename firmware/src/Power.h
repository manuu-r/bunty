#pragma once

#include "Runtime.h"

class Preferences;

namespace power {

// kDance is both servos making small, rate-capped moves while Bunty speaks.
enum class Load : uint8_t { kTurnServo, kTiltServo, kAudio, kDance, kCount };

enum class Mode : uint8_t {
  kNormal,
  kConservative,
};

void begin(Preferences &storage);

// False means starting this load would exceed the rail; the caller must skip it.
bool claim(Load load);
void release(Load load);
bool held(Load load);

void service();

void markBusy();

// Modem sleep adds a DTIM interval of latency to every packet; a live voice
// session keeps the radio awake while this is set.
void setRadioAwake(bool awake);

uint8_t backlightCeiling();
float servoSpeed(float requestedDegreesPerSecond);

Mode mode();
const char *modeName();
uint16_t committedMilliamps();
bool recoveredFromBrownout();

}  // namespace power
