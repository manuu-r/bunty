#include "AudioIo.h"

#include <driver/i2s.h>
#include <esp_heap_caps.h>

#include "Config.h"
#include "InternalAllocation.h"
#include "Log.h"
#include "Memory.h"

namespace {

using namespace cfg::audio;

constexpr size_t kReadFrames = kSampleRate / 50;  // 20 ms
constexpr size_t kChunkFrames = 128;

int16_t clip16(int32_t value) {
  if (value > INT16_MAX) return INT16_MAX;
  if (value < INT16_MIN) return INT16_MIN;
  return static_cast<int16_t>(value);
}

// Removes the capsule's DC offset: a one-pole high-pass at roughly 5 Hz, kept
// in Q8 so the tracked offset does not stall on integer truncation.
int32_t removeDc(int32_t sample, int32_t &state) {
  state += ((sample << 8) - state) >> 9;
  return sample - (state >> 8);
}

}  // namespace

// ------------------------------------------------------------- Microphone ---

bool Microphone::begin() {
  if (available_) return true;
  auto *storage = static_cast<uint8_t *>(memory::external(kCaptureRingBytes + 1));
  if (!storage) {
    Serial.println("[mic] could not allocate the capture ring");
    return false;
  }
  ring_ = xStreamBufferCreateStatic(kCaptureRingBytes + 1, 1, storage, &ringControl_);

  memory::InternalAllocationScope internal;
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 160;
  if (i2s_driver_install(cfg::kMicrophonePort, &config, 0, nullptr) != ESP_OK) {
    Serial.println("[mic] could not install the I2S driver");
    return false;
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = cfg::pin::kMicBclk;
  pins.ws_io_num = cfg::pin::kMicWs;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = cfg::pin::kMicData;
  if (i2s_set_pin(cfg::kMicrophonePort, &pins) != ESP_OK) {
    Serial.println("[mic] could not route the I2S pins");
    i2s_driver_uninstall(cfg::kMicrophonePort);
    return false;
  }
  available_ = true;
  if (xTaskCreatePinnedToCore(task, "mic", 4096, this, 6, nullptr, 1) != pdPASS) {
    available_ = false;
    i2s_driver_uninstall(cfg::kMicrophonePort);
    return false;
  }
  Serial.println("[mic] ready");
  return true;
}

void Microphone::task(void *context) {
  auto *mic = static_cast<Microphone *>(context);
  uint32_t reportedAt = millis();
  for (;;) {
    mic->readOnce();
    if (millis() - reportedAt >= 10000) {
      reportedAt = millis();
      memory::sampleTask("mic");
      const uint32_t dropped = mic->dropped_.exchange(0);
      if (dropped) logPrintf("[mic] capture ring overflow, %u bytes dropped\n", unsigned(dropped));
    }
  }
}

void Microphone::readOnce() {
  static int32_t slots[kReadFrames * 2];
  static int16_t mono[kReadFrames];
  size_t bytesRead = 0;
  if (i2s_read(cfg::kMicrophonePort, slots, sizeof(slots), &bytesRead,
               pdMS_TO_TICKS(100)) != ESP_OK || bytesRead == 0) {
    return;
  }
  const size_t frames = bytesRead / (2 * sizeof(int32_t));
  for (size_t i = 0; i < frames; ++i) {
    const int32_t left = removeDc(slots[2 * i] >> cfg::audio::kMicShift, dcLeft_);
    const int32_t right = removeDc(slots[2 * i + 1] >> cfg::audio::kMicShift, dcRight_);
    // Slowly tracked per-slot energy decides which capsules are really wired.
    energyLeft_ += uint64_t(int64_t(left) * left) - (energyLeft_ >> 12);
    energyRight_ += uint64_t(int64_t(right) * right) - (energyRight_ >> 12);
    int32_t mixed;
    if (energyLeft_ * kMicSlotRatio < energyRight_) mixed = right;
    else if (energyRight_ * kMicSlotRatio < energyLeft_) mixed = left;
    else mixed = (left + right) / 2;
    mono[i] = clip16(mixed);
    levelSum_ += uint64_t(int64_t(mono[i]) * mono[i]);
  }
  levelCount_ += frames;
  if (levelCount_ >= kReadFrames) {
    level_ = static_cast<uint32_t>(sqrtf(static_cast<float>(levelSum_ / levelCount_)));
    levelSum_ = 0;
    levelCount_ = 0;
  }
  if (!capturing_ || frames == 0) return;
  const size_t bytes = frames * sizeof(int16_t);
  const size_t sent = xStreamBufferSend(ring_, mono, bytes, 0);
  if (sent != bytes) dropped_ += bytes - sent;
}

size_t Microphone::read(uint8_t *destination, size_t bytes, uint32_t waitMs) {
  if (!available_) return 0;
  return xStreamBufferReceive(ring_, destination, bytes, pdMS_TO_TICKS(waitMs));
}

void Microphone::discard() {
  if (!available_) return;
  uint8_t scratch[256];
  while (xStreamBufferReceive(ring_, scratch, sizeof(scratch), 0) > 0) {
  }
}

size_t Microphone::buffered() const {
  return available_ ? xStreamBufferBytesAvailable(ring_) : 0;
}

// ---------------------------------------------------------------- Speaker ---

bool Speaker::open() {
  if (open_) return true;
  if (!installed_) {
    memory::InternalAllocationScope internal;
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = kSampleRate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = kSpeakerDmaBuffers;
    config.dma_buf_len = kSpeakerDmaFrames;
    // An underrun between sentences plays silence, not the last buffer looped.
    config.tx_desc_auto_clear = true;
    if (i2s_driver_install(cfg::kSpeakerPort, &config, 0, nullptr) != ESP_OK) {
      Serial.println("[speaker] could not install the I2S driver");
      return false;
    }
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = cfg::pin::kAmpBclk;
    pins.ws_io_num = cfg::pin::kAmpLrc;
    pins.data_out_num = cfg::pin::kAmpDin;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(cfg::kSpeakerPort, &pins) != ESP_OK) {
      Serial.println("[speaker] could not route the I2S pins");
      i2s_driver_uninstall(cfg::kSpeakerPort);
      return false;
    }
    installed_ = true;
  } else if (i2s_start(cfg::kSpeakerPort) != ESP_OK) {
    return false;
  }
  i2s_zero_dma_buffer(cfg::kSpeakerPort);
  digitalWrite(cfg::pin::kAmpShutdown, HIGH);
  delay(5);  // MAX98357 leaves shutdown in about 1 ms; don't clip the first word
  open_ = true;
  return true;
}

void Speaker::close() {
  if (!open_) return;
  digitalWrite(cfg::pin::kAmpShutdown, LOW);
  i2s_zero_dma_buffer(cfg::kSpeakerPort);
  i2s_stop(cfg::kSpeakerPort);
  open_ = false;
}

bool Speaker::write(const int16_t *samples, size_t count, uint8_t volumePercent) {
  if (!open_) return false;
  int16_t stereo[kChunkFrames * 2];
  size_t done = 0;
  while (done < count) {
    const size_t frames = min(kChunkFrames, count - done);
    for (size_t i = 0; i < frames; ++i) {
      const int16_t value = clip16(int32_t(samples[done + i]) * volumePercent / 100);
      stereo[2 * i] = value;
      stereo[2 * i + 1] = value;
    }
    size_t written = 0;
    const size_t bytes = frames * 2 * sizeof(int16_t);
    if (i2s_write(cfg::kSpeakerPort, stereo, bytes, &written, pdMS_TO_TICKS(500)) != ESP_OK ||
        written != bytes) {
      return false;
    }
    done += frames;
  }
  return true;
}
