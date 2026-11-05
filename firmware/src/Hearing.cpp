#include "Hearing.h"

#include <atomic>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Config.h"
#include "GccPhat.h"
#include "Log.h"
#include "Memory.h"

namespace hearing {
namespace {

constexpr size_t kWindow = cfg::hearing::kWindowSamples;

struct Window {
  int16_t left[kWindow];
  int16_t right[kWindow];
};

Window *windows = nullptr;  // two, in PSRAM: one filling while one is analysed
float *work = nullptr, *twiddle = nullptr, *hann = nullptr;
GccPhat gcc;
TaskHandle_t worker = nullptr;

// Producer (microphone task) state.
size_t filled = 0;
int filling = 0;
std::atomic<bool> dropFilled{false};
std::atomic<uint32_t> heldUntil{0};
// Index of the window waiting for the worker, or -1. The producer never writes
// to it, and drops a finished window rather than wait if the worker is busy.
std::atomic<int> ready{-1};
std::atomic<bool> resetRequested{false};

portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
Bearing published;
bool havePublished = false;

TrackerParams trackerParams() {
  TrackerParams params;
  params.sampleRate = float(cfg::audio::kMicSampleRate);
  params.spacingMm = cfg::mic::kSpacingMm;
  params.speedOfSoundMmPerMs = cfg::mic::kSpeedOfSoundMmPerMs;
  params.minRms = cfg::hearing::kMinimumRms;
  params.maxLevelRatio = cfg::hearing::kMaxLevelRatio;
  params.minCoherence = cfg::hearing::kMinimumCoherence;
  params.agreeDegrees = cfg::hearing::kAgreeDegrees;
  params.minAgreeing = cfg::hearing::kMinimumAgreeing;
  params.freshMs = cfg::hearing::kFreshMs;
  params.swapChannels = cfg::mic::kSwapChannels;
  return params;
}

BearingTracker tracker{trackerParams()};

bool loudEnough(const Window &window) {
  uint64_t left = 0, right = 0;
  for (size_t i = 0; i < kWindow; ++i) {
    left += uint64_t(int32_t(window.left[i]) * int32_t(window.left[i]));
    right += uint64_t(int32_t(window.right[i]) * int32_t(window.right[i]));
  }
  const uint64_t minimum = uint64_t(cfg::hearing::kMinimumRms) * cfg::hearing::kMinimumRms * kWindow;
  return left >= minimum && right >= minimum;
}

void workerTask(void *) {
  uint32_t reportedAt = 0;
  float reported = 1000.0f;
  uint32_t sampled = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    if (resetRequested.exchange(false)) tracker.reset();
    const int index = ready.load();
    if (index >= 0) {
      const uint32_t now = millis();
      // Silence and room tone cannot give a direction; skip the transforms.
      if (loudEnough(windows[index])) {
        const auto result = gcc.estimate(windows[index].left, windows[index].right);
        // A reset during the transform means this result describes the old head pose.
        if (!resetRequested.load() && tracker.add(result, now)) {
          Bearing heard;
          if (tracker.bearing(&heard, now)) {
            taskENTER_CRITICAL(&lock);
            published = heard;
            havePublished = true;
            taskEXIT_CRITICAL(&lock);
            if (now - reportedAt >= 500 && fabsf(heard.degrees - reported) >= 5.0f) {
              reportedAt = now;
              reported = heard.degrees;
              logPrintf("[hearing] source %+.0f deg, confidence %u\n", double(heard.degrees),
                        unsigned(heard.confidence));
            }
          }
        }
      }
      ready.store(-1);
    }
    if (millis() - sampled >= 5000) {
      sampled = millis();
      memory::sampleTask("hearing");
    }
  }
}

}  // namespace

bool begin() {
  if (!cfg::hearing::kEnabled) return false;
  if (worker) return true;
  constexpr size_t fft = cfg::hearing::kFftSize;
  windows = static_cast<Window *>(memory::external(2 * sizeof(Window)));
  work = static_cast<float *>(memory::external(2 * fft * sizeof(float)));
  twiddle = static_cast<float *>(memory::external(fft * sizeof(float)));
  hann = static_cast<float *>(memory::external(kWindow * sizeof(float)));
  const bool bound = windows && work && twiddle && hann &&
      gcc.bind(kWindow, fft, float(cfg::audio::kMicSampleRate), cfg::hearing::kBandLowHz,
               cfg::hearing::kBandHighHz, tracker.maxLagSamples(), work, twiddle, hann);
  // Below normal priority: a late angle matters less than a late sentence.
  if (!bound || xTaskCreate(workerTask, "hearing", 4096, nullptr, 3, &worker) != pdPASS) {
    worker = nullptr;
    heap_caps_free(windows);
    heap_caps_free(work);
    heap_caps_free(twiddle);
    heap_caps_free(hann);
    windows = nullptr;
    work = twiddle = hann = nullptr;
    Serial.println("[hearing] unavailable; voice capture continues without direction");
    return false;
  }
  logPrintf("[hearing] %.0f mm spacing gives +/-%.1f samples at %u Hz\n",
            double(cfg::mic::kSpacingMm), double(tracker.maxLagSamples()),
            unsigned(cfg::audio::kMicSampleRate));
  return true;
}

bool active() { return worker != nullptr; }

void gap() { dropFilled = true; }

void hold() { heldUntil = millis() + cfg::hearing::kSettleMs; }

void push(const int16_t *left, const int16_t *right, size_t frames) {
  if (!worker) return;
  if (dropFilled.exchange(false)) filled = 0;
  if (int32_t(millis() - heldUntil.load()) < 0) {
    filled = 0;
    return;
  }
  while (frames) {
    const size_t take = frames < kWindow - filled ? frames : kWindow - filled;
    memcpy(windows[filling].left + filled, left, take * sizeof(int16_t));
    memcpy(windows[filling].right + filled, right, take * sizeof(int16_t));
    filled += take;
    left += take;
    right += take;
    frames -= take;
    if (filled < kWindow) continue;
    filled = 0;
    if (ready.load() < 0) {
      ready.store(filling);
      filling ^= 1;
      xTaskNotifyGive(worker);
    }
  }
}

bool bearing(Bearing *out) {
  if (!worker) return false;
  taskENTER_CRITICAL(&lock);
  const Bearing copy = published;
  const bool have = havePublished;
  taskEXIT_CRITICAL(&lock);
  if (!have || int32_t(millis() - copy.at) > int32_t(cfg::hearing::kFreshMs)) return false;
  if (out) *out = copy;
  return true;
}

void reset() {
  taskENTER_CRITICAL(&lock);
  havePublished = false;
  taskEXIT_CRITICAL(&lock);
  resetRequested = true;
  dropFilled = true;
}

}  // namespace hearing
