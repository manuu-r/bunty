#pragma once

#include <Arduino.h>

class Preferences;

namespace power {

enum class Load : uint8_t { kTurnServo, kTiltServo, kCount };

enum class Mode : uint8_t {
  kNormal,
  kConservative,
  kSaver,
};

void begin(Preferences &storage);

// False means starting this load would exceed the rail; the caller must skip it.
bool claim(Load load);
void release(Load load);
bool held(Load load);

void service();

void markBusy();

uint8_t backlightCeiling();
float servoSpeed(float requestedDegreesPerSecond);

Mode mode();
const char *modeName();
uint16_t committedMilliamps();
bool recoveredFromBrownout();

// Dead reckoning with no feedback: advisory, and only sheds load when
// cfg::power::kSaverPercent is non-zero.
uint8_t estimatedPercentRemaining();
void resetFuelGauge();

}  // namespace power
