#pragma once

#include "Runtime.h"
#include <atomic>
#include "AudioIo.h"
#include "Config.h"
#include "Emotion.h"
#include "Memory.h"

// Muse conversation: tap once, local VAD sends each utterance after a pause,
// and listening resumes after playback. Capture pauses while Bunty speaks.
class Voice {
 public:
  enum class Phase : uint8_t { Off, Connecting, Listening, Thinking, Speaking, Finishing };
  bool begin(Microphone *microphone, const char *ttsToken);
  bool ready() const;
  // Waiting: exit. Recording: send. Reply: interrupt and listen again.
  void toggle() { toggleRequested_ = true; }
  void end() {endRequested_=true;}
  Phase phase() const { return phase_; }
  bool speechDetected() const {return detected_;}
  bool active() const { return phase_ != Phase::Off; }
  static const char *phaseName(Phase phase);
  float outputLevel() const { return outputLevel_; }
  Emotion emotion() const { return Emotion::Neutral; }
  uint32_t inputLevel() const { return microphone_ ? microphone_->level() : 0; }
  const char *lastError() const { return lastError_; }
 private:
  static void voiceTask(void *context);
  static void ttsTask(void *context);
  void run();
  bool playTts(const char *text, uint32_t generation);
  void setPhase(Phase phase);
  Microphone *microphone_ = nullptr;
  Speaker speaker_;
  std::atomic<bool> speaking_{false};
  std::atomic<bool> ready_{false};
  std::atomic<bool> toggleRequested_{false};
  std::atomic<bool> endRequested_{false};
  std::atomic<bool> playbackFailed_{false};
  std::atomic<bool> detected_{false};
  std::atomic<Phase> phase_{Phase::Off};
  std::atomic<float> outputLevel_{0.0f};
  uint8_t *preRollStorage_ = nullptr;
  int16_t *utteranceStorage_ = nullptr;
  uint32_t phaseSince_ = 0;
  std::atomic<const char *> lastError_{""};
  char token_[48] = {};
};
