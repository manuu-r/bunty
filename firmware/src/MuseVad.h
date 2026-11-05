#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace muse {
// Energy VAD for 16 kHz PCM. Decisions use audio duration, not task scheduling.
// This is a noise gate, not a speech classifier; sustained noise can trigger it.
class Vad {
 public:
  enum class Event { None, Started, Ended };
  static constexpr size_t kPreRollFrames = 5120;  // 320 ms, including onset.
  static constexpr size_t kTailFrames = 4000;     // 250 ms kept after the last word.
  void reset() { onset_ = quiet_ = duration_ = 0; speaking_ = false; }
  bool speaking() const { return speaking_; }
  // Silence recorded past the last word: it only delays the upload.
  size_t excessQuiet() const { return quiet_ > kTailFrames ? quiet_ - kTailFrames : 0; }
  Event process(const int16_t *pcm, size_t frames) {
    if (!frames) return Event::None;
    // The S3 has a single-precision FPU only: double maths is emulated in
    // software, and this runs for every 20 ms block. Sum squares as integers.
    uint64_t energy = 0;
    for (size_t i = 0; i < frames; ++i) energy += uint64_t(int32_t(pcm[i]) * int32_t(pcm[i]));
    const float level = std::sqrt(float(energy / frames));
    const float threshold = std::max(300.0f, noise_ * (speaking_ ? 2.0f : 3.0f));
    const bool loud = level >= threshold;
    if (!speaking_) {
      if (loud) onset_ += frames;
      else {
        onset_ = 0;
        // About 1.3 seconds to follow a changed background, independent of chunks.
        const float alpha = 1.0f - std::exp(-float(frames) / 20000.0f);
        noise_ += alpha * (level - noise_);
      }
      if (onset_ >= 1600) {  // 100 ms rejects brief clicks.
        speaking_ = true; duration_ = onset_; quiet_ = 0;
        return Event::Started;
      }
    } else {
      duration_ += frames;
      quiet_ = loud ? 0 : quiet_ + frames;
      if (quiet_ >= 14400 || duration_ >= 480000) {  // 900 ms pause / 30 s cap.
        speaking_ = false;
        return Event::Ended;
      }
    }
    return Event::None;
  }
 private:
  float noise_ = 200;
  size_t onset_ = 0, quiet_ = 0, duration_ = 0;
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
