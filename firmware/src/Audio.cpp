#include "Log.h"
#include "Audio.h"

#include <esp_heap_caps.h>
#include <string.h>

#include "Config.h"
#include "Memory.h"
#include "InternalAllocation.h"
#include "TapInput.h"

namespace { std::atomic<size_t> decoderAllocatedBytes{0}; }
size_t mp3DecoderAllocatedBytes() { return decoderAllocatedBytes.load(); }

extern "C" {
#include "libhelix-mp3/mp3dec.h"
// The pinned Helix library otherwise routes decoder working state to PSRAM.
// Linker wrappers keep all decoder working state internal and allocate once
// during stream startup, before the playback loop runs.
void *__wrap_helix_malloc(int size) {
  void *pointer = size > 0 ? heap_caps_calloc(1, size, memory::kInternal) : nullptr;
  if (pointer) decoderAllocatedBytes += heap_caps_get_allocated_size(pointer);
  return pointer;
}
void __wrap_helix_free(void *pointer) {
  if (pointer) decoderAllocatedBytes -= heap_caps_get_allocated_size(pointer);
  heap_caps_free(pointer);
}
}

// The greeting, embedded as MP3 by audio/greeting.S.
extern "C" const uint8_t buntyGreetingMp3[];
extern "C" const uint8_t buntyGreetingMp3End[];

namespace {

// Mono frames are expanded into both I2S slots in chunks of this size, which
// keeps the staging buffer on the stack instead of the heap.
constexpr size_t kChunkFrames = 128;

// Helix emits at most this many samples per decoded frame.
constexpr size_t kMaxFrameSamples = MAX_NGRAN * MAX_NCHAN * MAX_NSAMP;

void *allocatePreferPsram(size_t bytes) {
  return ESP.getPsramSize() ? memory::external(bytes) : memory::internal(bytes);
}

bool speakerInstalled = false;

}  // namespace

// ---------------------------------------------------------------- Speaker ---

bool Speaker::begin(uint32_t sampleRate) {
  if (open_) return true;
  memory::InternalAllocationScope allocationScope;

  if (speakerInstalled) {
    if (i2s_set_sample_rates(cfg::kSpeakerPort, sampleRate) != ESP_OK ||
        i2s_start(cfg::kSpeakerPort) != ESP_OK) return false;
    i2s_zero_dma_buffer(cfg::kSpeakerPort);
    digitalWrite(cfg::pin::kAmpShutdown, HIGH);
    sampleRate_ = sampleRate;
    open_ = true;
    return true;
  }

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = sampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = cfg::audio::kDmaBufferCount;
  config.dma_buf_len = cfg::audio::kDmaBufferLength;
  config.tx_desc_auto_clear = true;
  if (i2s_driver_install(cfg::kSpeakerPort, &config, 0, nullptr) != ESP_OK) {
    Serial.println("[audio] could not install the speaker I2S driver");
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = cfg::pin::kAmpBclk;
  pins.ws_io_num = cfg::pin::kAmpLrc;
  pins.data_out_num = cfg::pin::kAmpDin;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(cfg::kSpeakerPort, &pins) != ESP_OK) {
    Serial.println("[audio] could not route I2S to the amplifier");
    i2s_driver_uninstall(cfg::kSpeakerPort);
    return false;
  }

  digitalWrite(cfg::pin::kAmpShutdown, HIGH);
  speakerInstalled = true;
  delay(10);
  sampleRate_ = sampleRate;
  open_ = true;
  return true;
}

void Speaker::end() {
  if (!open_) return;
  i2s_zero_dma_buffer(cfg::kSpeakerPort);
  digitalWrite(cfg::pin::kAmpShutdown, LOW);
  i2s_stop(cfg::kSpeakerPort);
  sampleRate_ = 0;
  open_ = false;
}

bool Speaker::setSampleRate(uint32_t sampleRate) {
  if (!open_ || sampleRate == sampleRate_) return open_;
  if (i2s_set_sample_rates(cfg::kSpeakerPort, sampleRate) != ESP_OK) {
    logPrintf("[audio] could not set the speaker to %u Hz\n",
                  static_cast<unsigned>(sampleRate));
    return false;
  }
  sampleRate_ = sampleRate;
  return true;
}

bool Speaker::write(const int16_t *samples, size_t frames, uint8_t channels,
                    int32_t volumePercent) {
  if (!open_ || !samples || frames == 0) return open_;

  int16_t chunk[kChunkFrames * 2];
  size_t done = 0;
  while (done < frames) {
    const size_t count = min(kChunkFrames, frames - done);
    for (size_t i = 0; i < count; ++i) {
      const size_t index = (done + i) * channels;
      int32_t value = samples[index];
      if (channels == 2) value = (value + samples[index + 1]) / 2;
      const int16_t output =
          static_cast<int16_t>(value * volumePercent / 100);
      chunk[i * 2] = output;
      chunk[i * 2 + 1] = output;
    }
    const size_t bytes = count * 2 * sizeof(int16_t);
    size_t written = 0;
    if (i2s_write(cfg::kSpeakerPort, chunk, bytes, &written, pdMS_TO_TICKS(250)) !=
            ESP_OK ||
        written != bytes) {
      Serial.println("[audio] I2S write failed");
      return false;
    }
    done += count;
  }
  return true;
}

uint32_t meanMagnitude(const int16_t *samples, size_t frames,
                       uint8_t channels) {
  if (!samples || frames == 0) return 0;
  uint64_t total = 0;
  for (size_t i = 0; i < frames; ++i) {
    int32_t value = samples[i * channels];
    if (channels == 2) value = (value + samples[i * channels + 1]) / 2;
    total += static_cast<uint32_t>(value < 0 ? -value : value);
  }
  return static_cast<uint32_t>(total / frames);
}

// -------------------------------------------------------------- Mp3Stream ---

bool Mp3Stream::begin(size_t inputCapacity) {
  if (ready()) return true;

  input_ = static_cast<uint8_t *>(allocatePreferPsram(inputCapacity));
  pcm_ = static_cast<int16_t *>(
      memory::internal(kMaxFrameSamples * sizeof(int16_t)));
  decoder_ = MP3InitDecoder();
  if (!input_ || !pcm_ || !decoder_) {
    Serial.println("[audio] could not allocate the MP3 decoder");
    end();
    return false;
  }
  capacity_ = inputCapacity;
  buffered_ = 0;
  decodeErrors_ = 0;
  return true;
}

void Mp3Stream::end() {
  if (decoder_) {
    MP3FreeDecoder(decoder_);
    decoder_ = nullptr;
  }
  free(input_);
  input_ = nullptr;
  free(pcm_);
  pcm_ = nullptr;
  capacity_ = 0;
  buffered_ = 0;
}

Mp3Stream::Status Mp3Stream::decode(Frame *out) {
  if (!ready() || buffered_ < 4) return Status::NeedMoreData;

  const int sync = MP3FindSyncWord(input_, static_cast<int>(buffered_));
  if (sync < 0) {
    // No header anywhere in the window. A trailing 0xFF may be the first byte
    // of the next frame's sync word, so keep only that.
    buffered_ = input_[buffered_ - 1] == 0xff ? 1 : 0;
    if (buffered_ == 1) input_[0] = 0xff;
    return Status::NeedMoreData;
  }
  if (sync > 0) {
    memmove(input_, input_ + sync, buffered_ - sync);
    buffered_ -= static_cast<size_t>(sync);
  }

  unsigned char *read = input_;
  int remaining = static_cast<int>(buffered_);
  const int result = MP3Decode(decoder_, &read, &remaining, pcm_, 0);
  const size_t consumed = buffered_ - static_cast<size_t>(remaining);

  if (result == ERR_MP3_INDATA_UNDERFLOW ||
      result == ERR_MP3_MAINDATA_UNDERFLOW) {
    // Helix retains any completed bit-reservoir data internally and moves
    // `read` past the bytes it consumed. Keep only what it still needs;
    // retaining the old frame would decode the same underflow forever.
    if (consumed > 0) {
      if (remaining > 0) memmove(input_, read, static_cast<size_t>(remaining));
      buffered_ = static_cast<size_t>(remaining);
    } else if (buffered_ == capacity_) {
      // A full window that yields nothing is corrupt. Drop a byte so the next
      // sync search can move past it.
      memmove(input_, input_ + 1, buffered_ - 1);
      --buffered_;
      Serial.println("[audio] MP3 input resynchronized");
    }
    return Status::NeedMoreData;
  }

  if (result != ERR_MP3_NONE) {
    const size_t drop = consumed > 0 ? consumed : 1;
    memmove(input_, input_ + drop, buffered_ - drop);
    buffered_ -= drop;
    if (decodeErrors_ < 3) {
      ++decodeErrors_;
      logPrintf("[audio] MP3 frame skipped (%d)\n", result);
    }
    return Status::Skipped;
  }

  if (remaining > 0) memmove(input_, read, static_cast<size_t>(remaining));
  buffered_ = static_cast<size_t>(remaining);

  MP3FrameInfo info = {};
  MP3GetLastFrameInfo(decoder_, &info);
  const uint8_t channels = info.nChans == 2 ? 2 : 1;
  const size_t frames = static_cast<size_t>(info.outputSamps) / channels;
  if (info.samprate < 8000 || info.samprate > 48000 || frames == 0 ||
      static_cast<size_t>(info.outputSamps) > kMaxFrameSamples) {
    logPrintf("[audio] unsupported MP3 frame: %d Hz, %d channels\n",
                  info.samprate, info.nChans);
    return Status::Skipped;
  }

  if (out) {
    out->samples = pcm_;
    out->frames = frames;
    out->channels = channels;
    out->sampleRate = static_cast<uint32_t>(info.samprate);
  }
  return Status::Decoded;
}

// --------------------------------------------------------------- greeting ---

namespace greeting {
namespace {

std::atomic<bool> playing_{false};
TapInput *taps_ = nullptr;

void play() {
  const size_t total = static_cast<size_t>(buntyGreetingMp3End -
                                           buntyGreetingMp3);
  if (total == 0) return;

  Mp3Stream stream;
  Speaker speaker;
  if (!stream.begin(cfg::audio::kGreetingInputBytes)) return;
  if (!speaker.begin(cfg::audio::kMicrophoneSampleRate)) {
    stream.end();
    return;
  }

  size_t offset = 0;
  size_t played = 0;
  while (true) {
    memory::sampleTask("greeting");
    const size_t room = min(stream.writable(), total - offset);
    if (room > 0) {
      memcpy(stream.writePointer(), buntyGreetingMp3 + offset, room);
      stream.commit(room);
      offset += room;
    }

    Mp3Stream::Frame frame;
    const Mp3Stream::Status status = stream.decode(&frame);
    if (status == Mp3Stream::Status::Decoded) {
      speaker.setSampleRate(frame.sampleRate);
      if (!speaker.write(frame.samples, frame.frames, frame.channels,
                         cfg::audio::kGreetingVolumePercent)) {
        break;
      }
      played += frame.frames;
      continue;
    }
    if (status == Mp3Stream::Status::Skipped) continue;
    // NeedMoreData with nothing left in flash means the clip is finished.
    if (offset >= total) break;
  }

  speaker.end();
  stream.end();
  logPrintf("[audio] greeting played %u frames\n",
                static_cast<unsigned>(played));
}

void task(void *) {
  play();
  if (taps_) {
    taps_->reset();
    taps_->suppressFor(1000);
  }
  playing_ = false;
  vTaskDelete(nullptr);
}

}  // namespace

bool start(TapInput *taps) {
  if (playing_) return true;
  taps_ = taps;
  playing_ = true;
  if (taps) {
    taps->reset();
    taps->suppressFor(1000);
  }
  if (xTaskCreatePinnedToCore(task, "greeting", 4096, nullptr, 3, nullptr, 1) !=
      pdPASS) {
    Serial.println("[audio] could not start the greeting task");
    playing_ = false;
    return false;
  }
  return true;
}

bool playing() { return playing_; }

}  // namespace greeting
