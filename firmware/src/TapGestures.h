#pragma once

#include <Arduino.h>

class TapInput;

// Turns the raw tap stream into the handful of intents the runtime acts on.
//
// Gestures are graded by absolute level and by headroom over the measured room
// noise, so a soft knock or a desk bump cannot start a conversation and only a
// deliberate rhythm can begin unpairing. The voice and unpair windows do not
// overlap: a quick firm double tap talks, three slow taps twice over unpairs.
class TapGestures {
 public:
  // Several can be true for one tap; the caller acts on each independently.
  struct Intents {
    bool armVoice = false;
    bool wake = false;
    bool promptUnpair = false;
    bool confirmUnpair = false;
    // The unpair prompt timed out and the dashboard should be restored.
    bool cancelPrompt = false;
    // Any accepted gesture, so the caller can restart its idle-sleep timer.
    bool interacted = false;
  };

  explicit TapGestures(TapInput &taps) : taps_(taps) {}

  // Abandons any partial gesture. Call whenever the display state changes.
  void reset();

  // While the dashboard is on screen. `voiceReady` gates the double-tap arm so
  // taps do nothing before voice has activated.
  Intents service(bool voiceReady);

  // While the display sleeps: only a firm wake tap is recognized.
  Intents serviceSleeping();

  // True while the confirmation screen owns the display.
  bool awaitingConfirmation() const {
    return state_ == State::AwaitingConfirmation;
  }

  // True while a gesture is partly collected, so the idle timer should hold.
  bool inProgress() const { return state_ != State::Idle || tapCount_ != 0; }

 private:
  enum class State : uint8_t {
    Idle,
    CollectingFirstGroup,
    WaitingForSecondGroup,
    CollectingSecondGroup,
    AwaitingConfirmation,
  };

  void expireStaleGesture(uint32_t now, Intents &intents);

  TapInput &taps_;
  State state_ = State::Idle;
  uint8_t tapCount_ = 0;
  uint32_t lastTapAt_ = 0;
  uint32_t promptedAt_ = 0;
  uint32_t armFirstTapAt_ = 0;
};
