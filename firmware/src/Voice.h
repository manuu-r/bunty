#pragma once

#include "Runtime.h"
#include <atomic>
#include "AudioIo.h"
#include "Config.h"
#include "Emotion.h"
#include "Memory.h"
#include "SpeechPictures.h"
#include "SpeechPlayer.h"

// Muse conversation: tap once, local VAD sends each utterance after a pause,
// and listening resumes after playback. Capture pauses while Bunty speaks.
// While it speaks, playback position is the clock for the face and the head.
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
  Emotion emotion() const { return emotion_; }
  // Muse reports it is still busy with this request, not merely silent.
  bool working() const;
  uint32_t inputLevel() const { return microphone_ ? microphone_->level() : 0; }
  const char *lastError() const { return lastError_; }
  // A picture whose sentence has started; main task, paired with releasePicture().
  bool takePicture(const uint8_t *&data, size_t &bytes) { return pictures_.take(data, bytes); }
  void releasePicture() { pictures_.release(); }
 private:
  static void voiceTask(void *context);
  static void ttsTask(void *context);
  static void audioTask(void *context);
  void run();
  bool playTts(const char *text, uint32_t generation);
  audio::TtsOutcome fetchSpeech(const char *text, uint32_t generation);
  void setPhase(Phase phase);
  // The speaker side of the player, on the audio task.
  struct SpeechIo {
    Voice &voice;
    uint32_t now();
    void wait(uint32_t ms);
    bool claim();
    void release();
    bool open();
    void close();
    bool write(const int16_t *pcm, size_t frames);
    void level(float value);
    void playing();
    void cue(uint8_t emotion, int8_t gesture, int8_t picture);
    uint32_t drainMs() const;
    float fullScale() const;
  };
  Microphone *microphone_ = nullptr;
  Speaker speaker_;
  SpeechIo speechIo_{*this};
  // Held here, not in PSRAM with its samples: internal RAM keeps its atomics safe.
  audio::SpeechPlayer<SpeechIo> player_{speechIo_};
  audio::SpeechPictures pictures_;
  std::atomic<bool> speaking_{false};
  std::atomic<bool> ready_{false};
  std::atomic<bool> toggleRequested_{false};
  std::atomic<bool> endRequested_{false};
  std::atomic<bool> playbackFailed_{false};
  std::atomic<bool> detected_{false};
  std::atomic<Phase> phase_{Phase::Off};
  std::atomic<float> outputLevel_{0.0f};
  std::atomic<Emotion> emotion_{Emotion::Neutral};
  uint8_t *preRollStorage_ = nullptr;
  int16_t *utteranceStorage_ = nullptr;
  uint32_t phaseSince_ = 0;
  std::atomic<const char *> lastError_{""};
  char token_[48] = {};
};
