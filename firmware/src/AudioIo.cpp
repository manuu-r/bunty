#include "AudioIo.h"

#include <driver/i2s_std.h>
#include <esp_heap_caps.h>

#include "Config.h"
#include "InternalAllocation.h"
#include "Log.h"
#include "Memory.h"

namespace {

using namespace cfg::audio;

constexpr size_t kReadFrames = kSampleRate / 50;  // 20 ms
constexpr size_t kChunkFrames = 128;

// Energy is tracked as 4096 x mean square. -12 dBFS sustained, in the
// post-shift domain where full scale is 2^18, marks a slot as noise.
constexpr uint64_t kBrokenSlotEnergy = 4096ull * 65536ull * 65536ull;

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
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(cfg::kMicrophonePort, I2S_ROLE_MASTER);
  channel.dma_desc_num = 6;
  channel.dma_frame_num = 160;
  i2s_std_config_t config = {};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  config.gpio_cfg.bclk = gpio_num_t(cfg::pin::kMicBclk);
  config.gpio_cfg.ws = gpio_num_t(cfg::pin::kMicWs);
  config.gpio_cfg.dout = I2S_GPIO_UNUSED;
  config.gpio_cfg.din = gpio_num_t(cfg::pin::kMicData);
  if (!ring_ || i2s_new_channel(&channel, nullptr, &channel_) != ESP_OK ||
      i2s_channel_init_std_mode(channel_, &config) != ESP_OK ||
      i2s_channel_enable(channel_) != ESP_OK) {
    Serial.println("[mic] could not initialize the I2S channel");
    if (channel_) i2s_del_channel(channel_);
    channel_ = nullptr;
    heap_caps_free(storage);
    ring_ = nullptr;
    return false;
  }
  available_ = true;
  if (xTaskCreatePinnedToCore(task, "mic", 4096, this, 6, nullptr, 1) != pdPASS) {
    available_ = false;
    i2s_channel_disable(channel_);
    i2s_del_channel(channel_);
    channel_ = nullptr;
    heap_caps_free(storage);
    ring_ = nullptr;
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
  // Which capsules are actually wired, reported only when it changes.
  static int8_t reportedSlots = -1;
  const int8_t slots_ = int8_t((energyLeft_ > kBrokenSlotEnergy ? 1 : 0) |
                                (energyRight_ > kBrokenSlotEnergy ? 2 : 0));
  if (slots_ != reportedSlots) {
    reportedSlots = slots_;
    logPrintf("[mic] left slot %s, right slot %s\n",
              (slots_ & 1) ? "NOISE (ignored)" : "ok", (slots_ & 2) ? "NOISE (ignored)" : "ok");
  }
  size_t bytesRead = 0;
  if (i2s_channel_read(channel_, slots, sizeof(slots), &bytesRead, 100) != ESP_OK || bytesRead == 0) {
    return;
  }
  const size_t frames = bytesRead / (2 * sizeof(int32_t));
  for (size_t i = 0; i < frames; ++i) {
    const int32_t left = removeDc(slots[2 * i] >> cfg::audio::kMicShift, dcLeft_);
    const int32_t right = removeDc(slots[2 * i + 1] >> cfg::audio::kMicShift, dcRight_);
    // Slowly tracked per-slot energy decides which capsules are really wired.
    energyLeft_ += uint64_t(int64_t(left) * left) - (energyLeft_ >> 12);
    energyRight_ += uint64_t(int64_t(right) * right) - (energyRight_ >> 12);
    // An undriven slot (a mic whose L/R strap floats or matches the other's)
    // reads as bus noise near full scale; real room audio never sits there.
    const bool leftBroken = energyLeft_ > kBrokenSlotEnergy;
    const bool rightBroken = energyRight_ > kBrokenSlotEnergy;
    int32_t mixed;
    if (leftBroken && !rightBroken) mixed = right;
    else if (rightBroken && !leftBroken) mixed = left;
    // Loud speech can push the real mic over the line too; bus noise is still
    // the louder of the two.
    else if (leftBroken && rightBroken) mixed = energyLeft_ < energyRight_ ? left : right;
    else if (energyLeft_ * kMicSlotRatio < energyRight_) mixed = right;
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
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(cfg::kSpeakerPort, I2S_ROLE_MASTER);
    channel.dma_desc_num = kSpeakerDmaBuffers;
    channel.dma_frame_num = kSpeakerDmaFrames;
    channel.auto_clear_after_cb = true;
    i2s_std_config_t config = {};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk = gpio_num_t(cfg::pin::kAmpBclk);
    config.gpio_cfg.ws = gpio_num_t(cfg::pin::kAmpLrc);
    config.gpio_cfg.dout = gpio_num_t(cfg::pin::kAmpDin);
    config.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (i2s_new_channel(&channel, &channel_, nullptr) != ESP_OK ||
        i2s_channel_init_std_mode(channel_, &config) != ESP_OK) {
      Serial.println("[speaker] could not initialize the I2S channel");
      if (channel_) i2s_del_channel(channel_);
      channel_ = nullptr;
      return false;
    }
    installed_ = true;
  }
  // A disabled channel retains DMA data; clear every descriptor before reuse
  // so interruption never replays a fragment of the previous answer.
  static const int16_t silence[kSpeakerDmaBuffers * kSpeakerDmaFrames * 2] = {};
  size_t loaded = 0;
  if (i2s_channel_preload_data(channel_, silence, sizeof(silence), &loaded) != ESP_OK ||
      loaded != sizeof(silence) || i2s_channel_enable(channel_) != ESP_OK) return false;
  digitalWrite(cfg::pin::kAmpShutdown, HIGH);
  delay(5);  // MAX98357 leaves shutdown in about 1 ms; don't clip the first word
  open_ = true;
  return true;
}

void Speaker::close() {
  if (!open_) return;
  digitalWrite(cfg::pin::kAmpShutdown, LOW);
  i2s_channel_disable(channel_);
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
    if (i2s_channel_write(channel_, stereo, bytes, &written, 500) != ESP_OK ||
        written != bytes) {
      return false;
    }
    done += frames;
  }
  return true;
}
