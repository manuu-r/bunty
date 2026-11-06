#pragma once

#include "Runtime.h"
#include <atomic>
#include "AudioIo.h"

// What a heard "Hey Buddy" tells the rest of the firmware.
struct WakeEvent {
  // False when the phrase was heard but its direction was not trustworthy: one
  // capsule dead, a diffuse room, or two mics that disagree. Do not turn then.
  bool bearingValid = false;
  // Degrees from where the head was pointing; positive is Bunty's right.
  float bearingDegrees = 0.0f;
  float confidence = 0.0f;
  float delaySamples = 0.0f;
};

// Listens for Espressif's "Hey Buddy" WakeNet model on the microphone tap while
// armed, and works out which way the voice came from using both channels.
class WakeWord {
 public:
  bool begin(Microphone *microphone);
  // The model loaded and the listener is running.
  bool available() const { return loaded_; }
  // Listen only while nothing else owns the microphone.
  void setArmed(bool armed) { armed_ = armed; }
  bool armed() const { return armed_; }
  // Returns true once per heard phrase.
  bool take(WakeEvent &event);

 private:
  static void task(void *context);
  void run();

  Microphone *microphone_ = nullptr;
  std::atomic<bool> loaded_{false};
  std::atomic<bool> armed_{false};
  std::atomic<bool> pending_{false};
  WakeEvent event_;
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
};
