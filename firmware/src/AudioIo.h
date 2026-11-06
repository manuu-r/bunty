#pragma once

#include "Runtime.h"
#include <atomic>
#include "CaptureGate.h"
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

// One 16 kHz sample from each capsule, plus the mono mix the gateway expects.
struct MicFrame {
  int16_t mono, left, right;
};

// The two INMP441-style microphones on one I2S bus, reduced to the 16 kHz mono
// linear16 the gateway expects. A reader task owns the port; while capture is
// on, PCM accumulates in a PSRAM ring so nothing said during a TLS handshake
// is lost. Independently, monitoring publishes every frame with both channels
// to a second ring for the wake-word listener.
class Microphone {
 public:
  bool begin();
  bool available() const { return available_; }

  void setCapturing(bool capturing);
  // Starts or stops the per-channel tap; false if its ring could not be allocated.
  bool setMonitoring(bool monitoring);
  // Blocks up to waitMs for frames; returns how many were copied.
  size_t readMonitor(MicFrame *destination, size_t frames, uint32_t waitMs);
  // True only while both capsules are really wired, so a bearing means something.
  bool stereo() const { return stereo_; }
  // Blocks up to waitMs for at least one byte; returns bytes copied.
  size_t read(uint8_t *destination, size_t bytes, uint32_t waitMs);
  // Only the ring's reader may call this.
  void discard();
  size_t buffered() const;

  // RMS of the most recent captured 20 ms; zero while capture is off.
  uint32_t level() const { return level_; }

 private:
  static void task(void *context);
  void readOnce();

  i2s_chan_handle_t channel_ = nullptr;
  StreamBufferHandle_t ring_ = nullptr;
  StaticStreamBuffer_t ringControl_ = {};
  StreamBufferHandle_t tap_ = nullptr;
  StaticStreamBuffer_t tapControl_ = {};
  std::atomic<bool> monitoring_{false};
  std::atomic<bool> stereo_{false};
  std::atomic<uint32_t> tapDropped_{0};
  std::atomic<bool> available_{false};
  audio::CaptureGate capture_;
  std::atomic<uint32_t> level_{0};
  std::atomic<uint32_t> dropped_{0};

  int32_t dcLeft_ = 0;
  int32_t dcRight_ = 0;
  uint64_t energyLeft_ = 0;
  uint64_t energyRight_ = 0;
  uint64_t levelSum_ = 0;
  uint32_t levelCount_ = 0;
};

// Silent companion: no speaker hardware installed.
class Speaker {
 public:
  bool open() { return false; }
  void close() {}
  bool isOpen() const { return false; }
  bool write(const int16_t *, size_t, uint8_t) { return false; }
};
