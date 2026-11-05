#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hearing {

// Radix-2 FFT on interleaved re/im floats, in place. The caller owns the
// twiddle table so it can live in PSRAM beside the work buffer.
class Fft {
 public:
  static bool powerOfTwo(size_t n) { return n >= 2 && (n & (n - 1)) == 0; }
  // 2 * (n / 2) floats: cos(2 pi k / n), -sin(2 pi k / n).
  static void twiddles(float *table, size_t n) {
    for (size_t k = 0; k < n / 2; ++k) {
      const float angle = 6.28318530718f * float(k) / float(n);
      table[2 * k] = std::cos(angle);
      table[2 * k + 1] = -std::sin(angle);
    }
  }
  static void forward(float *data, size_t n, const float *table) {
    for (size_t i = 1, j = 0; i < n; ++i) {
      size_t bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) {
        const float re = data[2 * i], im = data[2 * i + 1];
        data[2 * i] = data[2 * j];
        data[2 * i + 1] = data[2 * j + 1];
        data[2 * j] = re;
        data[2 * j + 1] = im;
      }
    }
    for (size_t length = 2; length <= n; length <<= 1) {
      const size_t half = length >> 1, step = n / length;
      for (size_t i = 0; i < n; i += length) {
        for (size_t k = 0; k < half; ++k) {
          const float wr = table[2 * k * step], wi = table[2 * k * step + 1];
          float *a = data + 2 * (i + k), *b = data + 2 * (i + k + half);
          const float tr = b[0] * wr - b[1] * wi, ti = b[0] * wi + b[1] * wr;
          b[0] = a[0] - tr;
          b[1] = a[1] - ti;
          a[0] += tr;
          a[1] += ti;
        }
      }
    }
  }
};

// Generalized cross-correlation with phase transform. The PHAT weight keeps
// only the phase of the cross spectrum, so the correlation peak stays sharp
// in a reverberant room where plain correlation smears across reflections.
//
// Two real windows share one complex FFT (left in the real part, right in the
// imaginary part), and the inverse transform reuses the forward routine, so a
// window costs two FFTs. All storage is caller-owned.
class GccPhat {
 public:
  static constexpr size_t kMaxLags = 40;
  struct Result {
    bool valid = false;
    // Positive: the right channel is a delayed copy of the left, so the
    // source is nearer the left microphone.
    float lagSamples = 0;
    // Peak height over its ceiling: 1 for a perfect delayed copy, about
    // 1/sqrt(bins) for noise.
    float coherence = 0;
    uint32_t leftRms = 0, rightRms = 0;
  };

  // work: 2 * fft floats, twiddle: fft floats, window: n floats.
  bool bind(size_t n, size_t fft, float sampleRate, float lowHz, float highHz,
            float maxLagSamples, float *work, float *twiddle, float *window) {
    ready_ = false;
    if (!work || !twiddle || !window || !Fft::powerOfTwo(fft) || n < 8 || n > fft / 2 ||
        sampleRate <= 0 || lowHz < 0 || highHz <= lowHz || maxLagSamples < 0)
      return false;
    const size_t reach = size_t(std::ceil(maxLagSamples)) + 2;
    if (2 * reach + 3 > kMaxLags) return false;
    low_ = size_t(std::ceil(lowHz * float(fft) / sampleRate));
    if (low_ < 1) low_ = 1;
    high_ = size_t(std::floor(highHz * float(fft) / sampleRate));
    if (high_ > fft / 2 - 1) high_ = fft / 2 - 1;
    if (high_ < low_) return false;
    n_ = n;
    fft_ = fft;
    reach_ = reach;
    work_ = work;
    twiddle_ = twiddle;
    window_ = window;
    Fft::twiddles(twiddle_, fft_);
    for (size_t i = 0; i < n_; ++i)
      window_[i] = 0.5f - 0.5f * std::cos(6.28318530718f * float(i) / float(n_));
    ready_ = true;
    return true;
  }

  Result estimate(const int16_t *left, const int16_t *right) {
    Result result;
    if (!ready_ || !left || !right) return result;
    uint64_t leftEnergy = 0, rightEnergy = 0;
    for (size_t i = 0; i < n_; ++i) {
      leftEnergy += uint64_t(int32_t(left[i]) * int32_t(left[i]));
      rightEnergy += uint64_t(int32_t(right[i]) * int32_t(right[i]));
      work_[2 * i] = float(left[i]) * window_[i];
      work_[2 * i + 1] = float(right[i]) * window_[i];
    }
    result.leftRms = uint32_t(std::sqrt(float(leftEnergy / n_)));
    result.rightRms = uint32_t(std::sqrt(float(rightEnergy / n_)));
    for (size_t i = 2 * n_; i < 2 * fft_; ++i) work_[i] = 0;
    Fft::forward(work_, fft_, twiddle_);

    // Bins far below the loudest are noise; their phase is random and
    // PHAT would raise them to full weight.
    float loudest = 0;
    for (size_t k = low_; k <= high_; ++k) {
      float re, im;
      cross(k, re, im);
      const float magnitude = re * re + im * im;
      if (magnitude > loudest) loudest = magnitude;
    }
    if (loudest <= 0) return result;
    const float threshold = loudest * 1e-6f;  // 60 dB below in power

    float used = 0;
    work_[0] = work_[1] = work_[fft_] = work_[fft_ + 1] = 0;
    for (size_t k = 1; k < fft_ / 2; ++k) {
      float re = 0, im = 0;
      if (k >= low_ && k <= high_) {
        cross(k, re, im);
        const float magnitude = re * re + im * im;
        if (magnitude > threshold) {
          const float scale = 1.0f / std::sqrt(magnitude);
          re *= scale;
          im *= scale;
          used += 1.0f;
        } else {
          re = im = 0;
        }
      }
      work_[2 * k] = re;
      work_[2 * k + 1] = im;
      work_[2 * (fft_ - k)] = re;
      work_[2 * (fft_ - k) + 1] = -im;
    }
    if (used < 8) return result;
    Fft::forward(work_, fft_, twiddle_);

    // The spectrum is A * conj(B) and the transform is forward (e^-j), so a
    // right channel delayed by t samples peaks at index +t.
    float cc[kMaxLags];
    const int span = int(reach_) + 1;
    const int count = 2 * span + 1;
    for (int i = 0; i < count; ++i) {
      const int lag = i - span;
      cc[i] = work_[2 * size_t((lag + int(fft_)) % int(fft_))];
    }
    int best = 1;
    for (int i = 1; i < count - 1; ++i)
      if (cc[i] > cc[best]) best = i;
    float lag = float(best - span);
    const float before = cc[best - 1], at = cc[best], after = cc[best + 1];
    const float divisor = before - 2.0f * at + after;
    if (divisor < 0) {
      const float offset = 0.5f * (before - after) / divisor;
      if (offset > -1.0f && offset < 1.0f) lag += offset;
    }
    result.valid = at > 0;
    result.lagSamples = lag;
    result.coherence = at > 0 ? at / (2.0f * used) : 0;
    return result;
  }

 private:
  // Cross spectrum of the two channels at bin k, unpacked from the shared FFT.
  void cross(size_t k, float &re, float &im) const {
    const float *z = work_ + 2 * k, *n = work_ + 2 * (fft_ - k);
    const float ar = 0.5f * (z[0] + n[0]), ai = 0.5f * (z[1] - n[1]);
    const float br = 0.5f * (z[1] + n[1]), bi = -0.5f * (z[0] - n[0]);
    re = ar * br + ai * bi;
    im = ai * br - ar * bi;
  }

  bool ready_ = false;
  size_t n_ = 0, fft_ = 0, low_ = 0, high_ = 0, reach_ = 0;
  float *work_ = nullptr, *twiddle_ = nullptr, *window_ = nullptr;
};

}  // namespace hearing
