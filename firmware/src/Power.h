#pragma once

#include <Arduino.h>

class Preferences;

namespace power {

enum class Load : uint8_t { kTurnServo, kTiltServo, kAudio, kCount };

enum class Mode : uint8_t {
  kNormal,
  kConservative,
  kSaver,
};

void begin(Preferences &storage);

// False means starting this load would exceed the rail; the caller must skip
// it. Audio outranks motion and pre-empts it, returning only once the servo
// has been told to stop and cfg::power::kRevokeSettleMs has passed.
bool claim(Load load);
void release(Load load);
bool held(Load load);

// Runs on the claiming task, not the holder's: stop the load, do not block.
using RevokeHandler = void (*)();
void onRevoked(Load load, RevokeHandler handler);

bool audioBusy();
void setAudioExpected(bool expected);

void service();

void markBusy();

uint8_t backlightCeiling();
int32_t volumeCeiling(int32_t requestedPercent);
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
