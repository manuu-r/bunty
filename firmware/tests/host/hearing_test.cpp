#include "Decimator.h"
#include "SoundBearing.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

constexpr float kRate = 48000.0f;
constexpr size_t kWindow = 1024, kFft = 2048;

// Speech-like: many partials between 150 Hz and 4.5 kHz with a slow envelope.
// Built in continuous time so the right channel can lag by a fractional sample.
struct Source {
  std::vector<float> freq, phase, amp;
  explicit Source(unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> f(150.0f, 4500.0f), p(0.0f, 6.2831853f), a(0.3f, 1.0f);
    for (int i = 0; i < 60; ++i) { freq.push_back(f(rng)); phase.push_back(p(rng)); amp.push_back(a(rng)); }
  }
  float at(double t) const {
    double sum = 0;
    for (size_t i = 0; i < freq.size(); ++i) sum += amp[i] * std::sin(6.283185307 * freq[i] * t + phase[i]);
    return float(sum);
  }
};

struct Pair { std::vector<int16_t> left, right; };

// Right is the left delayed by `lag` samples; optional echo and independent noise.
static Pair make(const Source &src, float lag, float noiseRms, float echo, unsigned seed, size_t start = 0) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> noise(0.0f, 1.0f);
  Pair out;
  const float gain = 2500.0f / 4.0f;
  for (size_t i = 0; i < kWindow; ++i) {
    const double t = double(start + i) / kRate, d = double(lag) / kRate;
    float l = src.at(t), r = src.at(t - d);
    if (echo > 0) { l += echo * src.at(t - 0.011); r += echo * src.at(t - d - 0.017); }
    out.left.push_back(int16_t(std::lround(l * gain + noise(rng) * noiseRms)));
    out.right.push_back(int16_t(std::lround(r * gain + noise(rng) * noiseRms)));
  }
  return out;
}

struct Rig {
  std::vector<float> work = std::vector<float>(2 * kFft), twiddle = std::vector<float>(kFft), window = std::vector<float>(kWindow);
  hearing::GccPhat gcc;
  explicit Rig(float maxLag = 8.42f) {
    assert(gcc.bind(kWindow, kFft, kRate, 300.0f, 5000.0f, maxLag, work.data(), twiddle.data(), window.data()));
  }
};

static void fftMatchesDft() {
  const size_t n = 16;
  std::vector<float> table(n), data(2 * n), ref(2 * n);
  hearing::Fft::twiddles(table.data(), n);
  std::mt19937 rng(3);
  std::normal_distribution<float> g;
  for (auto &v : data) v = g(rng);
  for (size_t k = 0; k < n; ++k) {
    double re = 0, im = 0;
    for (size_t j = 0; j < n; ++j) {
      const double a = -6.283185307 * double(k * j) / n;
      re += data[2 * j] * std::cos(a) - data[2 * j + 1] * std::sin(a);
      im += data[2 * j] * std::sin(a) + data[2 * j + 1] * std::cos(a);
    }
    ref[2 * k] = float(re); ref[2 * k + 1] = float(im);
  }
  hearing::Fft::forward(data.data(), n, table.data());
  for (size_t i = 0; i < 2 * n; ++i) assert(std::fabs(data[i] - ref[i]) < 1e-3f);
}

static void recoversDelay() {
  Rig rig;
  for (float lag : {-8.0f, -5.3f, -2.0f, 0.0f, 0.5f, 3.7f, 8.0f}) {
    const auto clean = make(Source(7), lag, 0.0f, 0.0f, 1);
    const auto result = rig.gcc.estimate(clean.left.data(), clean.right.data());
    assert(result.valid);
    if (std::fabs(result.lagSamples - lag) >= 0.15f) std::printf("clean lag %.2f -> %.3f\n", lag, result.lagSamples);
    assert(std::fabs(result.lagSamples - lag) < 0.15f);
    assert(result.coherence > 0.5f);
    // About 12 dB signal to noise, plus reflections from a small room. This
    // source has only 60 partials, far sparser than speech, so it is harsher.
    const auto rough = make(Source(7), lag, 600.0f, 0.4f, 2);
    const auto messy = rig.gcc.estimate(rough.left.data(), rough.right.data());
    assert(messy.valid);
    // 0.6 sample is about 4 degrees near the front, about 10 at the side.
    if (std::fabs(messy.lagSamples - lag) >= 0.6f) std::printf("noisy lag %.2f -> %.3f (coh %.2f)\n", lag, messy.lagSamples, messy.coherence);
    assert(std::fabs(messy.lagSamples - lag) < 0.6f);
  }
}

static void noiseIsNotABearing() {
  Rig rig;
  std::mt19937 rng(11);
  std::normal_distribution<float> g(0.0f, 2000.0f);
  std::vector<int16_t> a(kWindow), b(kWindow);
  for (size_t i = 0; i < kWindow; ++i) { a[i] = int16_t(g(rng)); b[i] = int16_t(g(rng)); }
  const auto result = rig.gcc.estimate(a.data(), b.data());
  assert(result.coherence < 0.12f);
  const auto silent = std::vector<int16_t>(kWindow, 0);
  assert(!rig.gcc.estimate(silent.data(), silent.data()).valid);
}

static float degreesFor(float lag) { return -std::asin(lag / 8.3965f) * 57.29578f; }

static void bearingAndGating() {
  Rig rig;
  hearing::BearingTracker tracker;
  assert(std::fabs(tracker.maxLagSamples() - 8.3965f) < 0.01f);
  hearing::Bearing heard;
  uint32_t now = 1000;
  // Sound nearer the left capsule (positive lag) is to Bunty's left (negative).
  const float lag = 4.19825f;  // half the maximum: 30 degrees
  bool published = false;
  for (int window = 0; window < 4; ++window, now += 100) {
    const auto pair = make(Source(20 + window), lag, 300.0f, 0.2f, 40 + window, size_t(window) * 5000);
    const auto result = rig.gcc.estimate(pair.left.data(), pair.right.data());
    const bool changed = tracker.add(result, now);
    // Three agreeing windows are needed before a bearing is trusted.
    if (window < 2) assert(!changed && !tracker.bearing(&heard, now));
    published |= changed;
  }
  assert(published && tracker.bearing(&heard, now));
  assert(std::fabs(heard.degrees - degreesFor(lag)) < 3.0f && heard.degrees < -25.0f);
  assert(heard.confidence > 20);
  assert(!tracker.bearing(&heard, now + 2000));  // stale

  // Mirror image, and the swap option for a differently strapped board.
  hearing::BearingTracker right;
  hearing::TrackerParams swapped; swapped.swapChannels = true;
  hearing::BearingTracker flipped(swapped);
  for (int window = 0; window < 4; ++window) {
    const auto pair = make(Source(30 + window), -lag, 300.0f, 0.2f, 50 + window, size_t(window) * 5000);
    const auto result = rig.gcc.estimate(pair.left.data(), pair.right.data());
    right.add(result, 5000 + 100 * window);
    flipped.add(result, 5000 + 100 * window);
  }
  assert(right.bearing(&heard, 5400) && heard.degrees > 25.0f);
  assert(flipped.bearing(&heard, 5400) && heard.degrees < -25.0f);

  // Gates: quiet, one dead capsule, incoherent, and a lag no pair of capsules
  // this far apart could hear.
  hearing::GccPhat::Result good; good.valid = true; good.lagSamples = 2; good.coherence = 0.5f;
  good.leftRms = good.rightRms = 2000;
  auto rejected = [&](hearing::GccPhat::Result r) {
    hearing::BearingTracker t;
    for (int i = 0; i < 6; ++i) t.add(r, 100 * i);
    return !t.bearing(nullptr, 600);
  };
  assert(!rejected(good));
  auto quiet = good; quiet.leftRms = 200; assert(rejected(quiet));
  auto dead = good; dead.rightRms = 300; assert(rejected(dead));
  auto vague = good; vague.coherence = 0.05f; assert(rejected(vague));
  auto reflection = good; reflection.lagSamples = 9.5f; assert(rejected(reflection));
  auto invalid = good; invalid.valid = false; assert(rejected(invalid));
  // Slightly beyond the geometric limit is measurement noise and clamps to the side.
  auto edge = good; edge.lagSamples = 8.8f;
  hearing::BearingTracker t; for (int i = 0; i < 4; ++i) t.add(edge, 100 * i);
  assert(t.bearing(&heard, 400) && heard.degrees < -85.0f);
  // A lone outlier among agreeing windows is outvoted.
  hearing::BearingTracker voted;
  auto other = good; other.lagSamples = -6.0f;
  voted.add(good, 0); voted.add(good, 100); voted.add(other, 200); voted.add(good, 300);
  assert(voted.bearing(&heard, 300) && std::fabs(heard.degrees - degreesFor(2)) < 1.0f);
}

static void headAim() {
  hearing::LookAt look;
  assert(!look.update(false, 10, 0));
  assert(!look.update(true, 5.0f, 0) && look.target() == 0);        // inside the deadzone
  assert(look.update(true, 14.0f, 1000) && look.target() == 14.0f);
  assert(!look.update(true, -10.0f, 1400) && look.target() == 14.0f);  // cooling down
  assert(look.update(true, -10.0f, 1900) && look.target() == -10.0f);
  assert(look.update(true, 80.0f, 3000) && look.target() == 18.0f);     // power-reservation clamp
  assert(!look.update(true, NAN, 5000));
  look.reset(); assert(look.target() == 0);

  hearing::LookParams onHead; onHead.micsOnHead = true;
  hearing::LookAt chase(onHead);
  assert(chase.update(true, 12.0f, 1000) && chase.target() == 12.0f);
  // Bearings are now relative to the turned head: 4 more degrees is inside the
  // deadzone, but 15 the other way is a real change of speaker.
  assert(!chase.update(true, 4.0f, 2000) && chase.target() == 12.0f);
  assert(chase.update(true, -15.0f, 2000) && chase.target() == -3.0f);
  assert(chase.update(true, 40.0f, 3000) && chase.target() == 18.0f);
  assert(!chase.update(true, 8.0f, 4000) && chase.target() == 18.0f);  // already at the limit
}

static float toneGain(audio::Decimator &d, float hz, unsigned factor) {
  d.reset();
  double in = 0, out = 0; size_t outputs = 0;
  const size_t total = 48000;
  for (size_t i = 0; i < total; ++i) {
    const int32_t x = int32_t(std::lround(10000.0 * std::sin(6.283185307 * hz * double(i) / 48000.0)));
    int32_t y;
    if (d.push(x, y) && i > 2000) { out += double(y) * y; ++outputs; }
    if (i > 2000) in += double(x) * x;
  }
  (void)factor;
  assert(outputs > 0);
  return float(std::sqrt((out / outputs) / (in / (total - 2001))));
}

static void decimatesWithoutAliasing() {
  audio::Decimator d;
  assert(!d.configure(0) && !d.configure(9) && d.configure(3));
  int count = 0; int32_t y;
  for (int i = 0; i < 4800; ++i) count += d.push(100, y);
  assert(count == 1600);
  for (float hz : {300.0f, 1000.0f, 3000.0f, 6000.0f}) {
    const float g = toneGain(d, hz, 3);
    if (std::fabs(g - 1.0f) >= 0.06f) std::printf("passband %.0f Hz gain %.3f\n", hz, g);
    assert(std::fabs(g - 1.0f) < 0.06f);
  }
  // 10 to 20 kHz would fold into 6 to 4 kHz and be heard as speech.
  for (float hz : {10000.0f, 12000.0f, 15000.0f, 20000.0f}) {
    const float g = toneGain(d, hz, 3);
    if (g >= 0.01f) std::printf("stopband %.0f Hz gain %.4f\n", hz, g);
    assert(g < 0.01f);  // more than 40 dB down
  }
  audio::Decimator same;
  assert(same.configure(1) && same.push(-1234, y) && y == -1234);
}

static void speedReport() {
  Rig rig;
  const auto pair = make(Source(9), 3.0f, 100.0f, 0.2f, 5);
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) rig.gcc.estimate(pair.left.data(), pair.right.data());
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 20;
  std::printf("GCC-PHAT window on this host: %.3f ms (informational)\n", ms);
}

int main() {
  fftMatchesDft();
  recoversDelay();
  noiseIsNotABearing();
  bearingAndGating();
  headAim();
  decimatesWithoutAliasing();
  speedReport();
  std::puts("Hearing checks passed");
}
