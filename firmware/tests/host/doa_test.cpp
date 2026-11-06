#include "Doa.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr float kSpacing = 0.02f;
constexpr uint32_t kRate = 16000;

// Deterministic broadband noise, optionally low-passed like a voice's body.
std::vector<float> source(size_t frames, uint32_t seed, float smooth) {
  std::vector<float> out(frames + 64);
  float state = 0;
  for (auto &sample : out) {
    seed = seed * 1664525u + 1013904223u;
    const float white = (float(seed >> 8) / float(1 << 24) - 0.5f) * 2.0f;
    state += (1.0f - smooth) * (white - state);
    sample = state;
  }
  return out;
}
// Windowed-sinc fractional delay: sample the source `delay` samples late.
float delayed(const std::vector<float> &s, size_t n, float delay) {
  float sum = 0;
  for (int k = -16; k <= 16; ++k) {
    const float position = float(n) - delay;
    const int tap = int(std::floor(position)) + k;
    if (tap < 0 || tap >= int(s.size())) continue;
    const float x = position - float(tap);
    const float sinc = std::fabs(x) < 1e-6f ? 1.0f : std::sin(3.14159265f * x) / (3.14159265f * x);
    const float window = 0.5f + 0.5f * std::cos(3.14159265f * x / 17.0f);
    sum += s[tap] * sinc * window;
  }
  return sum;
}
doa::Estimate measure(float degrees, float smooth, float amplitude, uint32_t seed = 7) {
  const size_t frames = 16000;
  const auto talker = source(frames + 64, seed, smooth);
  const float delay = kSpacing * std::sin(degrees * 3.14159265f / 180.0f) / doa::kSpeedOfSound * float(kRate);
  std::vector<int16_t> left(frames), right(frames);
  for (size_t n = 32; n < frames; ++n) {
    right[n] = int16_t(talker[n] * amplitude);
    left[n] = int16_t(delayed(talker, n, delay) * amplitude);  // the left capsule hears it later
  }
  return doa::bearingOfSpeech(left.data(), right.data(), frames, kSpacing, kRate, 0.2f);
}
}  // namespace

int main() {
  // Straight ahead and to either side, for white noise and for voice-like low-passed noise.
  for (float smooth : {0.0f, 0.6f}) {
    for (float truth : {0.f, 20.f, -20.f, 40.f, -40.f, 60.f, -60.f}) {
      const auto e = measure(truth, smooth, 8000.0f);
      std::printf("smooth %.1f truth %+5.0f -> valid %d bearing %+6.1f delay %+5.2f conf %.2f\n", smooth, truth, e.valid,
                  e.bearingDegrees, e.delaySamples, e.confidence);
      assert(e.valid);
      assert(std::fabs(e.bearingDegrees - truth) <= 8.0f);
      if (std::fabs(truth) >= 20.0f) assert((e.bearingDegrees > 0) == (truth > 0));  // side is always right
    }
  }
  // Sound arriving at the right mic first means a positive (right-hand) bearing.
  assert(measure(40, 0.0f, 8000.0f).bearingDegrees > 15.0f);
  assert(measure(-40, 0.0f, 8000.0f).bearingDegrees < -15.0f);
  // Silence, and two unrelated noises (a diffuse room), give no direction.
  std::vector<int16_t> quiet(16000, 0);
  assert(!doa::bearingOfSpeech(quiet.data(), quiet.data(), quiet.size(), kSpacing, kRate, 0.2f).valid);
  const auto a = source(16000, 1, 0.0f), b = source(16000, 99, 0.0f);
  std::vector<int16_t> left(16000), right(16000);
  for (size_t i = 0; i < left.size(); ++i) { left[i] = int16_t(a[i] * 8000); right[i] = int16_t(b[i] * 8000); }
  assert(!doa::bearingOfSpeech(left.data(), right.data(), left.size(), kSpacing, kRate, 0.2f).valid);
  // A quiet room's noise before and after the phrase must not outvote the phrase.
  {
    const auto talker = source(16000, 5, 0.3f), room = source(16000, 77, 0.0f);
    const float delay = kSpacing * std::sin(40.f * 3.14159265f / 180.f) / doa::kSpeedOfSound * float(kRate);
    std::vector<int16_t> l(16000), r(16000);
    for (size_t n = 32; n < 16000; ++n) {
      const bool speaking = n > 6000 && n < 11000;
      const float amp = speaking ? 8000.f : 0.f;
      r[n] = int16_t(talker[n] * amp + room[n] * 300);
      l[n] = int16_t(delayed(talker, n, delay) * amp + room[(n + 1234) % 16000] * 300);
    }
    const auto e = doa::bearingOfSpeech(l.data(), r.data(), l.size(), kSpacing, kRate, 0.2f);
    std::printf("phrase in room noise -> valid %d bearing %+6.1f\n", e.valid, e.bearingDegrees);
    assert(e.valid && e.bearingDegrees > 15.0f);
  }
  std::puts("Two-microphone bearing checks passed");
}
