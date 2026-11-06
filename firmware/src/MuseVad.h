#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace muse {
// Energy VAD for 16 kHz PCM. Decisions use audio duration, not task scheduling.
// This is a noise gate, not a speech classifier. Varying noise (music, a TV) can
// still trigger it, but perfectly steady noise (a fan, a hum, a servo's whine)
// is recognised after a few seconds and the utterance is dropped.
class Vad {
 public:
  // Rejected: an utterance that started was steady noise; discard its audio.
  enum class Event { None, Started, Ended, Rejected };
  static constexpr size_t kPreRollFrames = 5120;  // 320 ms, including onset.
  static constexpr size_t kTailFrames = 4000;     // 250 ms kept after the last word.
  static constexpr size_t kPauseFrames = 10560;   // 660 ms pause (survives 600 ms hesitation, responds fast)
  static constexpr size_t kOnsetFrames = 1600;       // 100 ms of speech-level sound
  static constexpr size_t kSteadyFrames = 48000;     // 3 s without a single pause
  static constexpr double kSteadyDeviation = 0.12;   // level varies under 12% of its mean
  // How much silence after the last word ends an utterance. Survives reset().
  void setPauseFrames(size_t frames) { pause_ = std::max(frames, kTailFrames + 1); }
  void reset() {
    onset_ = quiet_ = duration_ = loudRun_ = 0;
    speaking_ = false; meanLevel_ = deviation_ = 0;
  }
  bool speaking() const { return speaking_; }
  // Silence recorded past the last word: it only delays the upload.
  size_t excessQuiet() const { return quiet_ > kTailFrames ? quiet_ - kTailFrames : 0; }
  // `hold` marks audio recorded while the head's servos were moving: their whine
  // is not speech, so it neither starts an utterance nor teaches the noise floor.
  Event process(const int16_t *pcm, size_t frames, bool hold = false) {
    if (!frames) return Event::None;
    double energy = 0;
    for (size_t i = 0; i < frames; ++i) energy += double(pcm[i]) * pcm[i];
    const double level = std::sqrt(energy / frames);
    const double threshold = std::max(300.0, noise_ * (speaking_ ? 2.0 : 3.0));
    const bool loud = level >= threshold;
    if (!speaking_) {
      if (hold) { onset_ = 0; return Event::None; }
      if (loud) onset_ += frames;
      else {
        // A quiet frame cancels twice its length of onset, so a click is wiped
        // by the next frame while a dip inside a soft word does not lose it all.
        onset_ = onset_ > 2 * frames ? onset_ - 2 * frames : 0;
        // About 1.3 seconds to follow a changed background, independent of chunks.
        const double alpha = 1.0 - std::exp(-double(frames) / 20000.0);
        noise_ += alpha * (level - noise_);
      }
      if (onset_ >= kOnsetFrames) {  // 100 ms rejects brief clicks.
        speaking_ = true; duration_ = onset_; quiet_ = loudRun_ = 0;
        meanLevel_ = level; deviation_ = level;  // wide until the sound proves steady
        return Event::Started;
      }
    } else {
      duration_ += frames;
      quiet_ = loud ? 0 : quiet_ + frames;
      loudRun_ = loud ? loudRun_ + frames : 0;
      // Speech swings in level from word to word; a fan does not.
      const double alpha = 1.0 - std::exp(-double(frames) / 16000.0);
      deviation_ += alpha * (std::fabs(level - meanLevel_) - deviation_);
      meanLevel_ += alpha * (level - meanLevel_);
      if (loudRun_ >= kSteadyFrames && deviation_ < kSteadyDeviation * meanLevel_) {
        // Raise the floor above this sound so it cannot start another utterance.
        noise_ = std::max(noise_, meanLevel_ / 2.0);
        reset();
        return Event::Rejected;
      }
      if (quiet_ >= pause_ || duration_ >= 480000) {  // the pause (660 ms unless set) / 30 s cap.
        speaking_ = false;
        return Event::Ended;
      }
    }
    return Event::None;
  }
 private:
  size_t pause_ = kPauseFrames;
  double noise_ = 200;
  size_t onset_ = 0, quiet_ = 0, duration_ = 0, loudRun_ = 0;
  double meanLevel_ = 0, deviation_ = 0;
  bool speaking_ = false;
};

// Caller supplies PSRAM. Kept separate so startup buffering is host-testable.
class PreRoll {
 public:
  explicit PreRoll(int16_t *samples) : samples_(samples) {}
  void reset() { next_ = used_ = 0; }
  void push(const int16_t *pcm, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
      samples_[next_] = pcm[i]; next_ = (next_ + 1) % Vad::kPreRollFrames;
      used_ = std::min(used_ + 1, Vad::kPreRollFrames);
    }
  }
  template<class Send> void flush(Send send) {
    const size_t first = (next_ + Vad::kPreRollFrames - used_) % Vad::kPreRollFrames;
    const size_t head = std::min(used_, Vad::kPreRollFrames - first);
    if (head) send(samples_ + first, head);
    if (used_ > head) send(samples_, used_ - head);
    reset();
  }
 private:
  int16_t *samples_;
  size_t next_ = 0, used_ = 0;
};
}  // namespace muse
