#include "WakeWord.h"

#include <esp_heap_caps.h>
#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <model_path.h>

#include "Config.h"
#include "Doa.h"
#include "Log.h"
#include "Memory.h"
#include "Power.h"

namespace {

using namespace cfg::wake;

constexpr size_t kHistoryFrames = cfg::audio::kSampleRate * kHistoryMs / 1000;
constexpr size_t kReadFrames = 160;  // 10 ms per read from the tap

}  // namespace

bool WakeWord::begin(Microphone *microphone) {
  if (!kEnabled || loaded_ || !microphone || !microphone->available()) return false;
  microphone_ = microphone;
  // Core 0 shares with Wi-Fi and the voice task, which never run together with
  // this one: the listener is disarmed for the whole conversation.
  return xTaskCreatePinnedToCore(task, "wake", 10240, this, 3, nullptr, 0) == pdPASS;
}

bool WakeWord::take(WakeEvent &event) {
  if (!pending_.exchange(false)) return false;
  taskENTER_CRITICAL(&lock_);
  event = event_;
  taskEXIT_CRITICAL(&lock_);
  return true;
}

void WakeWord::task(void *context) {
  static_cast<WakeWord *>(context)->run();
  vTaskDelete(nullptr);
}

void WakeWord::run() {
  srmodel_list_t *models = esp_srmodel_init("model");
  char *name = models ? esp_srmodel_filter(models, ESP_WN_PREFIX, "heybuddy") : nullptr;
  if (!name) {
    Serial.println("[wake] \"Hey Buddy\" model not found in the model partition; flash srmodels.bin (wake word off)");
    return;
  }
  const esp_wn_iface_t *wn = esp_wn_handle_from_name(name);
  model_iface_data_t *model = wn ? wn->create(name, DET_MODE_90) : nullptr;
  if (!model) {
    logPrintf("[wake] could not create the %s model (wake word off)\n", name);
    return;
  }
  const int chunk = wn->get_samp_chunksize(model);
  if (wn->get_samp_rate(model) != int(cfg::audio::kSampleRate) || wn->get_channel_num(model) != 1 || chunk <= 0 ||
      chunk > 2048) {
    logPrintf("[wake] %s expects %d Hz x%d, chunk %d; this firmware feeds 16 kHz mono (wake word off)\n", name,
              wn->get_samp_rate(model), wn->get_channel_num(model), chunk);
    wn->destroy(model);
    return;
  }

  // Each channel's last second, written twice (at i and i + length) so the
  // newest second is always one contiguous run for the direction estimate.
  auto *leftHistory = static_cast<int16_t *>(memory::external(2 * kHistoryFrames * sizeof(int16_t)));
  auto *rightHistory = static_cast<int16_t *>(memory::external(2 * kHistoryFrames * sizeof(int16_t)));
  auto *samples = static_cast<int16_t *>(memory::internal(size_t(chunk) * sizeof(int16_t)));
  auto *frames = static_cast<MicFrame *>(memory::internal(kReadFrames * sizeof(MicFrame)));
  if (!leftHistory || !rightHistory || !samples || !frames) {
    Serial.println("[wake] no memory for the listener (wake word off)");
    heap_caps_free(leftHistory); heap_caps_free(rightHistory); heap_caps_free(samples); heap_caps_free(frames);
    wn->destroy(model);
    return;
  }
  loaded_ = true;
  logPrintf("[wake] listening for \"%s\" (%s, %d-sample chunks); mic spacing %.0f mm\n",
            esp_wn_wakeword_from_name(name), name, chunk, kMicSpacingMeters * 1000.0f);

  bool listening = false;
  size_t write = 0, filled = 0, fill = 0;
  uint32_t lastWakeAt = 0, lastBusyAt = 0;
  for (;;) {
    if (!armed_) {
      if (listening) {
        microphone_->setMonitoring(false);
        listening = false;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (!listening) {
      if (!microphone_->setMonitoring(true)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      listening = true;
      write = filled = fill = 0;
      wn->clean(model);
    }
    const uint32_t now = millis();
    if (now - lastBusyAt >= kBusyRefreshMs) {
      lastBusyAt = now;
      power::markBusy();  // keeps the CPU at full speed while armed
    }
    const size_t got = microphone_->readMonitor(frames, kReadFrames, 50);
    for (size_t i = 0; i < got; ++i) {
      leftHistory[write] = leftHistory[write + kHistoryFrames] = frames[i].left;
      rightHistory[write] = rightHistory[write + kHistoryFrames] = frames[i].right;
      write = (write + 1) % kHistoryFrames;
      if (filled < kHistoryFrames) ++filled;
      samples[fill++] = frames[i].mono;
      if (fill < size_t(chunk)) continue;
      fill = 0;
      if (wn->detect(model, samples) != WAKENET_DETECTED) continue;

      wn->clean(model);
      if (now - lastWakeAt < kRefractoryMs && lastWakeAt) continue;
      lastWakeAt = now;
      WakeEvent heard;
      if (microphone_->stereo() && filled >= doa::kBlockFrames * 4) {
        // `write` now points at the oldest of the last `filled` frames.
        const size_t start = filled < kHistoryFrames ? 0 : write;
        const int16_t *a = leftHistory + start, *b = rightHistory + start;
        const int16_t *bunty_left = kLeftSlotIsBuntyLeft ? a : b, *bunty_right = kLeftSlotIsBuntyLeft ? b : a;
        const auto estimate = doa::bearingOfSpeech(bunty_left, bunty_right, filled, kMicSpacingMeters,
                                                   cfg::audio::kSampleRate, kMinBearingConfidence);
        heard.bearingValid = estimate.valid;
        heard.bearingDegrees = estimate.bearingDegrees;
        heard.confidence = estimate.confidence;
        heard.delaySamples = estimate.delaySamples;
      }
      logPrintf("[wake] heard \"Hey Buddy\": bearing %s %+.0f deg (right positive), correlation %.2f, delay %+.2f samples, stereo %d\n",
                heard.bearingValid ? "valid" : "unusable", heard.bearingDegrees, heard.confidence, heard.delaySamples,
                int(microphone_->stereo()));
      taskENTER_CRITICAL(&lock_);
      event_ = heard;
      taskEXIT_CRITICAL(&lock_);
      pending_ = true;
    }
  }
}
