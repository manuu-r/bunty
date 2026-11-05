#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace audio {
// Integer-factor low-pass and downsample, for turning the 48 kHz capture the
// direction finder wants into the 16 kHz mono the voice path was built for.
// A windowed-sinc FIR, 22 taps per output phase: flat to about 6.5 kHz and 50 dB
// down by 9.5 kHz, so nothing above the output's Nyquist folds into speech.
// Factor 1 passes samples through untouched.
class Decimator {
 public:
  static constexpr size_t kTapsPerPhase = 22;
  static constexpr unsigned kMaxFactor = 4;
  static constexpr size_t kMaxTaps = kTapsPerPhase * kMaxFactor;

  bool configure(unsigned factor) {
    if (factor < 1 || factor > kMaxFactor) return false;
    factor_ = factor;
    taps_ = factor == 1 ? 1 : kTapsPerPhase * factor;
    if (factor == 1) {
      coefficients_[0] = 1.0f;
    } else {
      const float cutoff = 0.5f / float(factor);  // cycles per input sample
      const float middle = float(taps_ - 1) * 0.5f;
      float sum = 0;
      for (size_t i = 0; i < taps_; ++i) {
        const float x = float(i) - middle;
        const float arg = 6.28318530718f * cutoff * x;
        const float sinc = x == 0 ? 2.0f * cutoff : std::sin(arg) / (3.14159265359f * x);
        const float hamming = 0.54f - 0.46f * std::cos(6.28318530718f * float(i) / float(taps_ - 1));
        coefficients_[i] = sinc * hamming;
        sum += coefficients_[i];
      }
      for (size_t i = 0; i < taps_; ++i) coefficients_[i] /= sum;
    }
    reset();
    return true;
  }
  unsigned factor() const { return factor_; }
  void reset() {
    for (float &value : history_) value = 0;
    position_ = 0;
    phase_ = 0;
  }
  // One input sample. Returns true when `out` holds an output sample.
  bool push(int32_t in, int32_t &out) {
    if (factor_ == 1) {
      out = in;
      return true;
    }
    position_ = position_ + 1 == taps_ ? 0 : position_ + 1;
    history_[position_] = history_[position_ + taps_] = float(in);
    if (++phase_ < factor_) return false;
    phase_ = 0;
    float sum = 0;
    const float *newest = history_ + position_ + taps_;
    for (size_t i = 0; i < taps_; ++i) sum += coefficients_[i] * newest[-ptrdiff_t(i)];
    out = int32_t(std::lrintf(sum));
    return true;
  }

 private:
  float coefficients_[kMaxTaps] = {1.0f};
  float history_[2 * kMaxTaps] = {};
  size_t taps_ = 1, position_ = 0;
  unsigned factor_ = 1, phase_ = 0;
};
}  // namespace audio
