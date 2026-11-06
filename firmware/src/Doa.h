#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Direction of a sound from the time difference between two microphones.
//
// With the capsules a few centimetres apart the whole usable delay is under
// two samples at 16 kHz, so the estimate is coarse: reliable for "left, right
// or ahead" and good to roughly 10-15 degrees near the front, worse toward the
// sides. Two mics also cannot tell front from back; callers assume the talker
// is in front.
namespace doa {

constexpr float kSpeedOfSound = 343.0f;  // m/s at room temperature
constexpr int kSearchLags = 3;           // whole-sample delays searched each way
constexpr int kPhases = 8;               // fractional steps per sample (1/8)
constexpr int kHalfTaps = 8;             // sinc interpolator reaches this far each side
constexpr size_t kBlockFrames = 320;     // 20 ms at 16 kHz

struct Estimate {
  bool valid = false;
  // Degrees from straight ahead; positive is the right-hand side, the side of
  // the right channel's microphone.
  float bearingDegrees = 0.0f;
  float delaySamples = 0.0f;  // left lags right by this much when positive
  float confidence = 0.0f;    // mean normalised correlation at the peak, 0..1
};

class Correlator {
 public:
  static constexpr int kSteps = 2 * kSearchLags * kPhases + 1;  // delays -3 .. +3 in 1/8 steps

  Correlator(float spacingMeters, uint32_t sampleRate) : spacing_(spacingMeters), rate_(sampleRate) {
    // Windowed-sinc fractional delays: filter[p][t] delays a signal by p/8 sample.
    for (int phase = 0; phase < kPhases; ++phase) {
      for (int tap = 0; tap < 2 * kHalfTaps; ++tap) {
        const float x = float(tap - (kHalfTaps - 1)) - float(phase) / float(kPhases);
        const float sinc = std::fabs(x) < 1e-6f ? 1.0f : std::sin(3.14159265f * x) / (3.14159265f * x);
        const float window = 0.5f + 0.5f * std::cos(3.14159265f * x / float(kHalfTaps));
        filter_[phase][tap] = sinc * window;
      }
    }
  }

  void reset() {
    for (float &sum : sum_) sum = 0.0f;
    blocks_ = 0;
  }

  // Adds audio from both microphones. Each block of up to 20 ms contributes its
  // normalised correlation, so a loud block counts no more than a quiet one.
  void add(const int16_t *left, const int16_t *right, size_t frames) {
    for (size_t at = 0; at < frames; at += kBlockFrames) addBlock(left + at, right + at, std::min(kBlockFrames, frames - at));
  }

  Estimate estimate(float minConfidence) const {
    Estimate out;
    if (!blocks_ || spacing_ <= 0.0f || rate_ == 0) return out;
    float mean[kSteps];
    int best = 0;
    for (int i = 0; i < kSteps; ++i) {
      mean[i] = sum_[i] / float(blocks_);
      if (mean[i] > mean[best]) best = i;
    }
    // A peak on the edge of the search window is noise or a reflection, not a
    // delay the capsule spacing allows.
    if (best == 0 || best == kSteps - 1) return out;
    const float before = mean[best - 1], peak = mean[best], after = mean[best + 1];
    const float curve = before - 2.0f * peak + after;
    // The fine grid is dense enough that a parabola through the peak is accurate.
    const float offset = curve < -1e-9f ? std::max(-0.5f, std::min(0.5f, 0.5f * (before - after) / curve)) : 0.0f;
    const float delay = (float(best - kSearchLags * kPhases) + offset) / float(kPhases);
    const float maxDelay = spacing_ * float(rate_) / kSpeedOfSound;
    out.confidence = peak;
    out.delaySamples = delay;
    // The true delay cannot exceed the spacing; allow a little for estimation error.
    if (peak < minConfidence || std::fabs(delay) > maxDelay + 0.5f) return out;
    const float ratio = std::max(-1.0f, std::min(1.0f, delay / maxDelay));
    out.bearingDegrees = std::asin(ratio) * 57.2957795f;
    out.valid = true;
    return out;
  }

 private:
  void addBlock(const int16_t *left, const int16_t *right, size_t frames) {
    // First difference whitens speech a little, so the low-frequency body of a
    // voice does not flatten the correlation peak. Held as int16 to keep this
    // frame small; a differenced full-scale sample saturates rather than wraps.
    int16_t a[kBlockFrames], b[kBlockFrames], shifted[kBlockFrames];
    float previousLeft = 0.0f, previousRight = 0.0f;
    float leftEnergy = 0.0f, rightEnergy = 0.0f;
    for (size_t i = 0; i < frames; ++i) {
      a[i] = saturate(float(left[i]) - 0.95f * previousLeft);
      b[i] = saturate(float(right[i]) - 0.95f * previousRight);
      previousLeft = float(left[i]);
      previousRight = float(right[i]);
      leftEnergy += float(a[i]) * float(a[i]);
      rightEnergy += float(b[i]) * float(b[i]);
    }
    const float norm = std::sqrt(leftEnergy * rightEnergy);
    if (norm < 1.0f) return;
    const int count = int(frames);
    for (int phase = 0; phase < kPhases; ++phase) {
      // shifted[n] = right[n - phase/8]: the right channel delayed a fraction.
      for (int n = 0; n < count; ++n) {
        float sum = 0.0f;
        for (int tap = 0; tap < 2 * kHalfTaps; ++tap) {
          const int m = n - (tap - (kHalfTaps - 1));
          if (m >= 0 && m < count) sum += filter_[phase][tap] * float(b[m]);
        }
        shifted[n] = saturate(sum);
      }
      // Then whole-sample lags on top of it give delays of lag + phase/8.
      for (int lag = -kSearchLags; lag <= kSearchLags; ++lag) {
        const int step = lag * kPhases + phase + kSearchLags * kPhases;
        if (step < 0 || step >= kSteps) continue;
        float sum = 0.0f;
        for (int n = std::max(0, lag); n < count + std::min(0, lag); ++n) sum += float(a[n]) * float(shifted[n - lag]);
        sum_[step] += sum / norm;
      }
    }
    ++blocks_;
  }

  static int16_t saturate(float value) {
    return int16_t(std::max(-32768.0f, std::min(32767.0f, value)));
  }

  float spacing_;
  uint32_t rate_;
  float filter_[kPhases][2 * kHalfTaps];
  float sum_[kSteps] = {};
  uint32_t blocks_ = 0;
};

// Bearing of the loudest part of a recording, e.g. the last second of audio
// around a spoken phrase. Blocks well below the loudest one are mostly room
// noise, whose arrival time says nothing about the talker.
inline Estimate bearingOfSpeech(const int16_t *left, const int16_t *right, size_t frames, float spacingMeters,
                                uint32_t sampleRate, float minConfidence, float minBlockRms = 150.0f) {
  const size_t blocks = frames / kBlockFrames;
  float peakRms = 0.0f;
  float rms[64] = {};
  const size_t counted = std::min<size_t>(blocks, 64);
  for (size_t block = 0; block < counted; ++block) {
    double energy = 0.0;
    for (size_t i = 0; i < kBlockFrames; ++i) {
      const double l = left[block * kBlockFrames + i], r = right[block * kBlockFrames + i];
      energy += l * l + r * r;
    }
    rms[block] = float(std::sqrt(energy / (2.0 * kBlockFrames)));
    peakRms = std::max(peakRms, rms[block]);
  }
  Correlator correlator(spacingMeters, sampleRate);
  const float gate = std::max(minBlockRms, 0.4f * peakRms);
  for (size_t block = 0; block < counted; ++block)
    if (rms[block] >= gate) correlator.add(left + block * kBlockFrames, right + block * kBlockFrames, kBlockFrames);
  return correlator.estimate(minConfidence);
}

}  // namespace doa
