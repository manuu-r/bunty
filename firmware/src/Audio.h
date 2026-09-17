#pragma once

#include <Arduino.h>
#include <driver/i2s.h>

class TapInput;

// Everything that makes sound leave the MAX98357, plus the MP3 decoder both
// the boot greeting and the gateway's spoken replies share.

// The amplifier's I2S transmitter. The driver is installed only while audio is
// first needed; DMA storage is retained across turns to avoid heap churn.
// The transmitter stops and the amp's shutdown pin is low while idle.
//
// SD tied high makes the MAX98357 average both slots, so mono samples are
// duplicated into left and right to keep full output level. That expansion
// happens in a small internal chunk buffer; callers never stage one.
class Speaker {
 public:
  bool begin(uint32_t sampleRate);
  void end();
  bool open() const { return open_; }

  // Retunes the running transmitter. The decoder reports each stream's real
  // rate, which may differ from the rate begin() was given.
  bool setSampleRate(uint32_t sampleRate);
  uint32_t sampleRate() const { return sampleRate_; }

  // Blocks until the DMA has accepted every frame. `channels` may be 1 or 2;
  // stereo input is downmixed. Returns false if the transmitter rejected a
  // write.
  bool write(const int16_t *samples, size_t frames, uint8_t channels,
             int32_t volumePercent);

 private:
  bool open_ = false;
  uint32_t sampleRate_ = 0;
};

// Mean sample magnitude, used to drive Bunty's mouth from the audio actually
// reaching the amplifier rather than from a timer.
uint32_t meanMagnitude(const int16_t *samples, size_t frames, uint8_t channels);
size_t mp3DecoderAllocatedBytes();

// A Helix MP3 decoder with its own compressed-input window. The decoder itself
// ships prebuilt with the ESP32 framework; this owns the buffer bookkeeping
// that surrounds it -- sync search, bit-reservoir underflow, and resynchronizing
// past a corrupt frame -- so callers only push bytes and pull frames.
class Mp3Stream {
 public:
  struct Frame {
    const int16_t *samples = nullptr;  // interleaved
    size_t frames = 0;                 // per channel
    uint8_t channels = 1;
    uint32_t sampleRate = 0;
  };

  enum class Status : uint8_t {
    Decoded,       // `frame` holds PCM.
    NeedMoreData,  // Push more compressed bytes and call again.
    Skipped,       // A frame was unusable and dropped; call again.
  };

  ~Mp3Stream() { end(); }

  // Compressed input uses PSRAM when present; decoded PCM stays internal.
  bool begin(size_t inputCapacity);
  void end();
  bool ready() const { return decoder_ != nullptr; }

  // Compressed input. Write at most writable() bytes at writePointer(), then
  // commit exactly what was produced.
  size_t writable() const { return capacity_ - buffered_; }
  uint8_t *writePointer() { return input_ + buffered_; }
  void commit(size_t bytes) { buffered_ += bytes; }
  size_t buffered() const { return buffered_; }
  void discardInput() { buffered_ = 0; }

  Status decode(Frame *out);

 private:
  void *decoder_ = nullptr;
  uint8_t *input_ = nullptr;
  int16_t *pcm_ = nullptr;
  size_t capacity_ = 0;
  size_t buffered_ = 0;
  uint8_t decodeErrors_ = 0;
};

// Plays the embedded greeting on its own task so loop() stays free to animate.
// The clip is loud enough to trip the tap detector, so capture is suppressed
// for its duration and briefly afterwards.
namespace greeting {
bool start(TapInput *taps);
bool playing();
}  // namespace greeting
