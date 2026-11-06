#pragma once

#include "Runtime.h"
#include <atomic>
#include "AudioIo.h"
#include "Config.h"
#include "Emotion.h"
#include "Memory.h"

// Non-verbal Bunty companion:
// Input: Microphones + Local Energy VAD capture user speech.
// Output: Pure Pantomime via Head Servos (Yaw/Pitch) and LCD Display (Eyes/Emotions/Captions/GIFs).
// Zero human speech synthesis, zero audio playout latency, zero speaker hardware required.
class Voice {
 public:
  enum class Phase : uint8_t { Off, Connecting, Listening, Thinking, Speaking, Finishing };
  bool begin(Microphone *microphone, const char *token);
  bool ready() const;
  bool gatewayAvailable() const;
  // Waiting: exit. Recording: send. Reaction: interrupt and listen again.
  void toggle() { toggleRequested_ = true; }
  // Opens a conversation whose first turn is patient: see cfg::wake::kSilenceEndMs.
  void startFromWake() { wakeTurn_ = true; toggleRequested_ = true; }
  void end() { endRequested_ = true; }
  Phase phase() const { return phase_; }
  bool speechDetected() const { return detected_; }
  bool active() const { return phase_ != Phase::Off; }
  static const char *phaseName(Phase phase);
  float outputLevel() const { return 0.0f; } // silent companion: no mouth-energy flapping
  Emotion emotion() const { return emotion_; }
  void setEmotion(Emotion e) { emotion_ = e; }
  bool working() const;
  uint32_t inputLevel() const { return microphone_ ? microphone_->level() : 0; }
  const char *lastError() const { return lastError_; }
  bool takePicture(const uint8_t *&, size_t &) { return false; }
  void releasePicture() {}
  void react(const char *replyText);
  // Call while the head's servos are moving: their whine is not speech, so the
  // detector ignores the microphone until a moment after they stop.
  void noteMotion(uint32_t now) { motionUntil_ = now + 300; }

 private:
  static void voiceTask(void *context);
  void run();
  void setPhase(Phase phase);
  bool fetchGatewayReply(const int16_t *pcm, size_t frames, char *replyBuf, size_t replyBufSize);

  Microphone *microphone_ = nullptr;
  std::atomic<bool> ready_{false};
  std::atomic<bool> toggleRequested_{false};
  std::atomic<bool> wakeTurn_{false};
  std::atomic<bool> endRequested_{false};
  std::atomic<bool> detected_{false};
  std::atomic<Phase> phase_{Phase::Off};
  std::atomic<uint32_t> motionUntil_{0};
  std::atomic<Emotion> emotion_{Emotion::Neutral};
  uint8_t *preRollStorage_ = nullptr;
  int16_t *gateStorage_ = nullptr;
  int16_t *utteranceStorage_ = nullptr;
  uint32_t phaseSince_ = 0;
  uint32_t reactionUntil_ = 0;
  std::atomic<const char *> lastError_{""};
  char token_[48] = {};
};
