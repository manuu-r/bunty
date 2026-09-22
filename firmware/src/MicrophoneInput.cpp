#include "Log.h"
#include "MicrophoneInput.h"

#include <algorithm>
#include <esp_heap_caps.h>

#include "Config.h"
#include "SoundDirection.h"
#include "Memory.h"
#include "InternalAllocation.h"

namespace {

using namespace cfg::mic;

// One I2S read's worth of raw 32-bit slots. Both slots are read; only the one
// carrying the microphone is published as capture PCM.
constexpr size_t kReadSlots = 96;


}  // namespace

bool MicrophoneInput::begin() {
  if (available_) return true;
  memory::InternalAllocationScope allocationScope;
  captureMutex_ = xSemaphoreCreateMutexStatic(&captureMutexControl_);
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  // Both microphones share the data line, one per half-frame.
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 6;
  config.dma_buf_len = 64;
  if (i2s_driver_install(cfg::kMicrophonePort, &config, 0, nullptr) != ESP_OK) {
    Serial.println("[mic] could not install microphone I2S driver");
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = cfg::pin::kMicBclk;
  pins.ws_io_num = cfg::pin::kMicWs;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = cfg::pin::kMicData;
  if (i2s_set_pin(cfg::kMicrophonePort, &pins) != ESP_OK) {
    Serial.println("[mic] could not route microphone I2S pins");
    i2s_driver_uninstall(cfg::kMicrophonePort);
    return false;
  }

  available_ = true;
  readerRunning_ = true;
  if (xTaskCreatePinnedToCore(readerTask, "microphone", 3072, this, 2, nullptr,
                              1) != pdPASS) {
    Serial.println("[mic] could not start reader task");
    available_ = false;
    readerRunning_ = false;
    i2s_driver_uninstall(cfg::kMicrophonePort);
    return false;
  }
  Serial.println("[mic] microphone pair ready");
  return true;
}

void MicrophoneInput::end() {
  if (!available_) return;
  stopCapture();
  // Let the reader retire before the I2S driver is torn down underneath it.
  available_ = false;
  while (readerRunning_) delay(1);
  i2s_driver_uninstall(cfg::kMicrophonePort);
}

void MicrophoneInput::readFrames() {
  if (!available_) return;

  int32_t slots[kReadSlots];
  size_t bytesRead = 0;
  // Pace continuous stereo capture from DMA, including sound direction.
  const TickType_t wait = pdMS_TO_TICKS(20);
  if (i2s_read(cfg::kMicrophonePort, slots, sizeof(slots), &bytesRead, wait) !=
          ESP_OK ||
      bytesRead == 0) {
    return;
  }

  const size_t count = bytesRead / sizeof(slots[0]);
  hearing::observe(slots, count, millis());
  if (!capturing_) return;

  int16_t mono[kReadSlots / 2];
  size_t monoCount = 0;
  for (size_t i = 0; i < count; ++i) {
    // Both halves carry a microphone; only one feeds the gateway.
    if (static_cast<uint8_t>(i & 1) != cfg::mic::kVoiceSlot) continue;
    // 24 bits left-aligned in the slot, down to the linear16 the gateway wants.
    const int32_t amplified = (slots[i] >> 16) * kVoiceCaptureGain;
    mono[monoCount++] = static_cast<int16_t>(std::max<int32_t>(
        INT16_MIN, std::min<int32_t>(INT16_MAX, amplified)));
  }

  // Dropping on a full ring is deliberate: a stalled consumer must not block
  // the reader.
  if (monoCount > 0 && captureStream_) {
    const size_t bytes = monoCount * sizeof(mono[0]);
    const size_t sent = xStreamBufferSend(captureStream_, mono, bytes, 0);
    captureDroppedBytes_ += bytes - sent;
  }
}

bool MicrophoneInput::startCapture(size_t ringBytes) {
  if (!available_ || capturing_) return capturing_;
  if (ringBytes < 2 || ringBytes == SIZE_MAX || (ringBytes & 1)) return false;

  // PSRAM keeps the ring away from the internal heap the display framebuffer,
  // Wi-Fi, and the TLS session are already competing for.
  captureStorage_ = static_cast<uint8_t *>(
      heap_caps_malloc(ringBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!captureStorage_) {
    Serial.println("[mic] could not allocate the capture ring");
    return false;
  }

  // Static stream buffers reserve one byte to distinguish full from empty.
  // The usable capacity must be even so a short write cannot split PCM16.
  captureStream_ = xStreamBufferCreateStatic(ringBytes + 1, 1, captureStorage_,
                                             &captureControl_);
  if (!captureStream_) {
    Serial.println("[mic] could not create the capture stream");
    free(captureStorage_);
    captureStorage_ = nullptr;
    return false;
  }

  xSemaphoreTake(captureMutex_, portMAX_DELAY);
  capturing_ = true;
  xSemaphoreGive(captureMutex_);
  logPrintf("[mic] microphone capture started (%u byte ring)\n",
                static_cast<unsigned>(ringBytes));
  return true;
}

void MicrophoneInput::stopCapture() {
  if (!capturing_) return;
  capturing_ = false;
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
  Serial.println("[mic] microphone capture stopped");
}

size_t MicrophoneInput::readCapture(void *destination, size_t bytes, uint32_t waitMs) {
  if (!capturing_ || !captureStream_) return 0;
  return xStreamBufferReceive(captureStream_, destination, bytes,
                              pdMS_TO_TICKS(waitMs));
}

void MicrophoneInput::readerTask(void *context) {
  auto *input = static_cast<MicrophoneInput *>(context);
  uint32_t sampled = 0;
  while (input->available_) {
    xSemaphoreTake(input->captureMutex_, portMAX_DELAY);
    input->readFrames();
    xSemaphoreGive(input->captureMutex_);
    // The blocking read paces this loop while capturing; sleeping as well
    // would drop microphone samples.
    if (!input->capturing_) delay(4);
    if (millis() - sampled >= 10000) {
      sampled = millis();
      memory::sampleTask("microphone");
      const uint32_t dropped = input->captureDroppedBytes_.exchange(0);
      if (dropped) logPrintf("[audio] capture_overrun_bytes=%u\n", unsigned(dropped));
    }
  }
  input->readerRunning_ = false;
  vTaskDelete(nullptr);
}
