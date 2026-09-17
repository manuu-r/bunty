#include "Log.h"
#include "TapInput.h"

#include <algorithm>
#include <esp_heap_caps.h>

#include "Config.h"
#include "Memory.h"
#include "InternalAllocation.h"

namespace {

using namespace cfg::tap;

// One I2S read's worth of raw 32-bit slots. Both slots are read; only the one
// carrying the microphone is published as capture PCM.
constexpr size_t kReadSlots = 96;

bool deadlinePassed(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace

bool TapInput::begin() {
  if (available_) return true;
  memory::InternalAllocationScope allocationScope;
  captureMutex_ = xSemaphoreCreateMutexStatic(&captureMutexControl_);
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  // A board may strap the microphone to either slot. Reading both lets the
  // level detector use whichever contains samples without a firmware option.
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 6;
  config.dma_buf_len = 64;
  if (i2s_driver_install(cfg::kMicrophonePort, &config, 0, nullptr) != ESP_OK) {
    Serial.println("[tap] could not install microphone I2S driver");
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = cfg::pin::kMicBclk;
  pins.ws_io_num = cfg::pin::kMicWs;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = cfg::pin::kMicData;
  if (i2s_set_pin(cfg::kMicrophonePort, &pins) != ESP_OK) {
    Serial.println("[tap] could not route microphone I2S pins");
    i2s_driver_uninstall(cfg::kMicrophonePort);
    return false;
  }

  tapQueue_ = xQueueCreateStatic(8, sizeof(TapEvent), tapQueueStorage_, &tapQueueControl_);
  if (!tapQueue_) {
    Serial.println("[tap] could not create detector queue");
    i2s_driver_uninstall(cfg::kMicrophonePort);
    return false;
  }

  available_ = true;
  detectorRunning_ = true;
  reset();
  if (xTaskCreatePinnedToCore(detectorTask, "tap-detector", 3072, this, 2,
                              nullptr, 1) != pdPASS) {
    Serial.println("[tap] could not start detector task");
    available_ = false;
    detectorRunning_ = false;
    vQueueDelete(tapQueue_);
    tapQueue_ = nullptr;
    i2s_driver_uninstall(cfg::kMicrophonePort);
    return false;
  }
  Serial.println("[tap] microphone detector ready");
  return true;
}

void TapInput::end() {
  if (!available_) return;
  stopCapture();
  // Let the detector task observe the flag and retire before its queue and the
  // I2S driver are torn down underneath it.
  available_ = false;
  while (detectorRunning_) delay(1);
  if (tapQueue_) {
    vQueueDelete(tapQueue_);
    tapQueue_ = nullptr;
  }
  i2s_driver_uninstall(cfg::kMicrophonePort);
}

uint32_t TapInput::readLevel() {
  if (!available_) return 0;

  int32_t slots[kReadSlots];
  size_t bytesRead = 0;
  // Capture cannot afford dropped samples, so the read blocks and the DMA
  // paces the detector loop. Tap-only operation keeps the original
  // poll-and-sleep cadence so wake and reset thresholds behave as before.
  const TickType_t wait = capturing_ ? pdMS_TO_TICKS(20) : 0;
  if (i2s_read(cfg::kMicrophonePort, slots, sizeof(slots), &bytesRead, wait) !=
          ESP_OK ||
      bytesRead == 0) {
    return 0;
  }

  const size_t count = bytesRead / sizeof(slots[0]);
  const bool capturing = capturing_;
  int16_t mono[kReadSlots / 2];
  size_t monoCount = 0;
  uint32_t peak = 0;

  for (size_t i = 0; i < count; ++i) {
    // Common I2S MEMS microphones deliver a signed 24-bit sample left-aligned
    // in this 32-bit slot. Shifting keeps the adaptive thresholds readable,
    // and makes negation safe for every value.
    const int32_t sample = slots[i] >> 8;
    const uint32_t magnitude =
        static_cast<uint32_t>(sample < 0 ? -sample : sample);
    peak = std::max(peak, magnitude);

    if (!capturing) continue;

    // I2S_CHANNEL_FMT_RIGHT_LEFT interleaves two slots per frame. While the
    // active slot is still unknown, total each one's energy instead of
    // publishing what may be a channel of silence.
    if (captureSlot_ < 0) {
      slotEnergy_[i & 1] += magnitude;
    } else if (static_cast<int8_t>(i & 1) == captureSlot_) {
      // 24-bit down to the linear16 the gateway expects.
      const int32_t amplified = (sample >> 8) * kVoiceCaptureGain;
      mono[monoCount++] = static_cast<int16_t>(std::max<int32_t>(
          INT16_MIN, std::min<int32_t>(INT16_MAX, amplified)));
    }
  }

  if (capturing && captureSlot_ < 0 && ++slotProbeReads_ >= kSlotProbeReads) {
    captureSlot_ = slotEnergy_[1] > slotEnergy_[0] ? 1 : 0;
    logPrintf("[tap] microphone on I2S slot %d\n",
                  static_cast<int>(captureSlot_));
  }

  // Dropping on a full ring is deliberate: a stalled consumer must not block
  // the detector task and stall tap detection with it.
  if (monoCount > 0 && captureStream_) {
    const size_t bytes = monoCount * sizeof(mono[0]);
    const size_t sent = xStreamBufferSend(captureStream_, mono, bytes, 0);
    captureDroppedBytes_ += bytes - sent;
  }
  return peak;
}

bool TapInput::startCapture(size_t ringBytes) {
  if (!available_ || capturing_) return capturing_;
  if (ringBytes < 2 || ringBytes == SIZE_MAX || (ringBytes & 1)) return false;

  // PSRAM keeps the ring away from the internal heap the display framebuffer,
  // Wi-Fi, and the TLS session are already competing for.
  captureStorage_ = static_cast<uint8_t *>(
      heap_caps_malloc(ringBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!captureStorage_) {
    Serial.println("[tap] could not allocate the capture ring");
    return false;
  }

  // Static stream buffers reserve one byte to distinguish full from empty.
  // The usable capacity must be even so a short write cannot split PCM16.
  captureStream_ = xStreamBufferCreateStatic(ringBytes + 1, 1, captureStorage_,
                                             &captureControl_);
  if (!captureStream_) {
    Serial.println("[tap] could not create the capture stream");
    free(captureStorage_);
    captureStorage_ = nullptr;
    return false;
  }

  xSemaphoreTake(captureMutex_, portMAX_DELAY);
  captureSlot_ = -1;
  slotEnergy_[0] = 0;
  slotEnergy_[1] = 0;
  slotProbeReads_ = 0;
  capturing_ = true;
  xSemaphoreGive(captureMutex_);
  logPrintf("[tap] microphone capture started (%u byte ring)\n",
                static_cast<unsigned>(ringBytes));
  return true;
}

void TapInput::stopCapture() {
  if (!capturing_) return;
  capturing_ = false;
  // The detector task may be inside readLevel(); let it finish before the
  // stream and its storage go away.
  // Voice joins its sole capture reader before calling this. Join the
  // producer's current read explicitly, rather than guessing its duration.
  xSemaphoreTake(captureMutex_, portMAX_DELAY);
  if (captureStream_) {
    vStreamBufferDelete(captureStream_);
    captureStream_ = nullptr;
  }
  free(captureStorage_);
  captureStorage_ = nullptr;
  xSemaphoreGive(captureMutex_);
  Serial.println("[tap] microphone capture stopped");
}

size_t TapInput::readCapture(void *destination, size_t bytes, uint32_t waitMs) {
  if (!capturing_ || !captureStream_) return 0;
  return xStreamBufferReceive(captureStream_, destination, bytes,
                              pdMS_TO_TICKS(waitMs));
}

bool TapInput::detectTap(TapEvent *event) {
  const uint32_t level = readLevel();
  if (level == 0) return false;

  const uint32_t now = millis();
  const uint32_t threshold =
      std::max(kMinimumLevel, noiseFloor_ * kNoiseMultiplier);

  // Only quiet samples influence the baseline; otherwise a knock would raise
  // its own threshold and make the next tap hard to detect.
  if (level < threshold) {
    noiseFloor_ = (noiseFloor_ * 31 + level) / 32;
  }

  if (level < threshold / 2) {
    if (quietSince_ == 0) quietSince_ = now;
    if (now - quietSince_ >= kQuietToRearmMs) armed_ = true;
  } else {
    quietSince_ = 0;
  }

  if (!armed_ || !deadlinePassed(now, suppressedUntil_) ||
      now - lastTapAt_ < kRefractoryMs || level < threshold) {
    return false;
  }

  armed_ = false;
  lastTapAt_ = now;
  logPrintf("[tap] impulse %u (floor %u)\n", static_cast<unsigned>(level),
                static_cast<unsigned>(noiseFloor_));
  if (event) {
    event->occurredAt = now;
    event->level = level;
    event->noiseFloor = noiseFloor_;
  }
  return true;
}

void TapInput::detectorTask(void *context) {
  auto *input = static_cast<TapInput *>(context);
  uint32_t sampled = 0;
  while (input->available_) {
    TapEvent event;
    xSemaphoreTake(input->captureMutex_, portMAX_DELAY);
    const bool detected = input->detectTap(&event);
    xSemaphoreGive(input->captureMutex_);
    if (detected) {
      xQueueSend(input->tapQueue_, &event, 0);
    }
    // The blocking read inside detectTap() paces the loop while capturing;
    // sleeping here as well would drop microphone samples.
    if (!input->capturing_) delay(4);
    if (millis() - sampled >= 10000) {
      sampled = millis();
      memory::sampleTask("tap-detector");
      const uint32_t dropped = input->captureDroppedBytes_.exchange(0);
      if (dropped) logPrintf("[audio] capture_overrun_bytes=%u\n", unsigned(dropped));
    }
  }
  input->detectorRunning_ = false;
  vTaskDelete(nullptr);
}

bool TapInput::poll(TapEvent *event) {
  if (!available_ || !tapQueue_) return false;
  TapEvent received;
  if (xQueueReceive(tapQueue_, &received, 0) != pdTRUE) return false;
  if (event) *event = received;
  return true;
}

TapDecision TapInput::waitForPairingDecision() {
  if (!available_) return TapDecision::TimedOut;

  reset();
  const uint32_t startedAt = millis();
  uint32_t firstTapAt = 0;
  while (true) {
    const uint32_t now = millis();
    if (now - startedAt >= kPairingDecisionTimeoutMs) {
      return TapDecision::TimedOut;
    }
    if (poll()) {
      if (firstTapAt == 0) {
        firstTapAt = now;
      } else if (now - firstTapAt <= kPairingDoubleTapWindowMs) {
        return TapDecision::Cancel;
      }
    }
    if (firstTapAt != 0 && now - firstTapAt > kPairingDoubleTapWindowMs) {
      return TapDecision::Confirm;
    }
    delay(8);
  }
}

void TapInput::reset() {
  if (!available_) return;
  if (tapQueue_) {
    TapEvent discarded;
    while (xQueueReceive(tapQueue_, &discarded, 0) == pdTRUE) {
    }
  }
  suppressFor(250);
}

void TapInput::suppressFor(uint32_t milliseconds) {
  suppressedUntil_ = millis() + milliseconds;
}
