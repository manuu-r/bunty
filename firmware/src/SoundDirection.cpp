#include "Log.h"
#include "SoundDirection.h"

#include <math.h>
#include <string.h>

#include "Config.h"

namespace hearing {
namespace {

using namespace cfg::hearing;

int16_t left_[kWindowSamples];
int16_t right_[kWindowSamples];
size_t filled_ = 0;

Bearing published_;
float candidate_ = 0.0f;
uint8_t agreeing_ = 0;

const float kMaximumLagSamples =
    cfg::mic::kSpacingMm / cfg::mic::kSpeedOfSoundMmPerMs *
    (cfg::audio::kMicrophoneSampleRate / 1000.0f);

// Positive lag means the sound reached the left microphone first.
int64_t correlate(int lag) {
  int64_t sum = 0;
  const size_t from = lag < 0 ? static_cast<size_t>(-lag) : 0;
  const size_t to = lag > 0 ? kWindowSamples - static_cast<size_t>(lag)
                            : kWindowSamples;
  for (size_t i = from; i < to; ++i) {
    sum += static_cast<int32_t>(left_[i]) * right_[i + lag];
  }
  return sum;
}

uint32_t windowRms(const int16_t *window) {
  uint64_t total = 0;
  for (size_t i = 0; i < kWindowSamples; ++i) {
    total += static_cast<int32_t>(window[i]) * window[i];
  }
  return static_cast<uint32_t>(sqrt(static_cast<double>(total / kWindowSamples)));
}

void evaluate(uint32_t now) {
  const uint32_t leftRms = windowRms(left_);
  const uint32_t rightRms = windowRms(right_);
  if (leftRms < kMinimumRms || rightRms < kMinimumRms) {
    agreeing_ = 0;
    return;
  }

  int bestLag = 0;
  int64_t best = INT64_MIN;
  int64_t neighbours[2 * kMaxLagSamples + 1] = {};
  for (int lag = -kMaxLagSamples; lag <= kMaxLagSamples; ++lag) {
    const int64_t value = correlate(lag);
    neighbours[lag + kMaxLagSamples] = value;
    if (value > best) {
      best = value;
      bestLag = lag;
    }
  }
  if (best <= 0) {
    agreeing_ = 0;
    return;
  }

  // Scoring against the window's own energy keeps a loud diffuse peak from
  // outranking a quiet clear one.
  const int64_t energy =
      static_cast<int64_t>(leftRms) * rightRms * kWindowSamples;
  const uint8_t confidence = energy > 0
      ? static_cast<uint8_t>(min<int64_t>(100, best * 100 / energy))
      : 0;
  if (confidence < kMinimumConfidence) {
    agreeing_ = 0;
    return;
  }

  // Without interpolation the usable range is five integer lags, so the head
  // could only ever point at five places.
  float lag = static_cast<float>(bestLag);
  const int index = bestLag + kMaxLagSamples;
  if (index > 0 && index < 2 * kMaxLagSamples) {
    const double before = static_cast<double>(neighbours[index - 1]);
    const double at = static_cast<double>(neighbours[index]);
    const double after = static_cast<double>(neighbours[index + 1]);
    const double divisor = before - 2.0 * at + after;
    if (divisor != 0.0) {
      const double offset = 0.5 * (before - after) / divisor;
      if (offset > -1.0 && offset < 1.0) lag += static_cast<float>(offset);
    }
  }

  // Rejected rather than clamped: a reflection must not read as a hard-left talker.
  if (fabsf(lag) > kMaximumLagSamples + 0.5f) {
    agreeing_ = 0;
    return;
  }

  float sine = lag / kMaximumLagSamples;
  if (sine > 1.0f) sine = 1.0f;
  if (sine < -1.0f) sine = -1.0f;
  float degrees = -asinf(sine) * 57.29578f;
  if (cfg::mic::kLeftSlot != 0) degrees = -degrees;

  if (agreeing_ > 0 && fabsf(degrees - candidate_) <= kAgreementDegrees) {
    candidate_ = (candidate_ + degrees) * 0.5f;
    if (++agreeing_ >= kAgreeingWindows) {
      published_.degrees = candidate_;
      published_.confidence = confidence;
      published_.at = now;
      agreeing_ = kAgreeingWindows;
    }
  } else {
    candidate_ = degrees;
    agreeing_ = 1;
  }
}

}  // namespace

void begin() {
  reset();
  logPrintf("[hearing] %.0f mm spacing gives +/-%.1f samples at %u Hz\n",
            static_cast<double>(cfg::mic::kSpacingMm),
            static_cast<double>(kMaximumLagSamples),
            static_cast<unsigned>(cfg::audio::kMicrophoneSampleRate));
}

void observe(const int32_t *slots, size_t count, uint32_t now) {
  if (!slots) return;

  for (size_t i = 0; i + 1 < count; i += 2) {
    // MEMS parts deliver 24 bits left-aligned in the 32-bit slot.
    const int32_t a = slots[i] >> 16;
    const int32_t b = slots[i + 1] >> 16;
    left_[filled_] = static_cast<int16_t>(cfg::mic::kLeftSlot == 0 ? a : b);
    right_[filled_] = static_cast<int16_t>(cfg::mic::kLeftSlot == 0 ? b : a);
    if (++filled_ < kWindowSamples) continue;
    filled_ = 0;
    evaluate(now);
  }
}

bool bearing(Bearing *out) {
  if (published_.at == 0 || millis() - published_.at > kFreshMs) return false;
  if (out) *out = published_;
  return true;
}

void reset() {
  published_ = Bearing{};
  candidate_ = 0.0f;
  agreeing_ = 0;
  filled_ = 0;
}

}  // namespace hearing
