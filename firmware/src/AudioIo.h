#pragma once

#include "Runtime.h"
#include <atomic>
#include "CaptureGate.h"
#include "Decimator.h"
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

// The two INMP441-style microphones on one I2S bus, sampled at cfg::audio::
// kMicSampleRate and reduced to the 16 kHz mono linear16 the gateway expects.
// A reader task owns the port; while capture is on, PCM accumulates in a PSRAM
// ring so nothing said during a TLS handshake is lost. The same task hands both
// raw channels to hearing:: for sound direction.
class Microphone {
 public:
  bool begin();
  bool available() const { return available_; }

  void setCapturing(bool capturing);
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
  std::atomic<bool> available_{false};
  audio::CaptureGate capture_;
  std::atomic<uint32_t> level_{0};
  std::atomic<uint32_t> dropped_{0};

  audio::Decimator decimator_;
  uint32_t decimatedSession_ = 0;
  int32_t dcLeft_ = 0;
  int32_t dcRight_ = 0;
  uint64_t energyLeft_ = 0;
  uint64_t energyRight_ = 0;
  uint64_t levelSum_ = 0;
  uint32_t levelCount_ = 0;
};

// MAX98357 on its own I2S port. The amplifier is shut down whenever the
// speaker is closed, so it cannot hiss or pick up the servos' PWM.
class Speaker {
 public:
  bool open();
  void close();
  bool isOpen() const { return open_; }
  // Mono 16 kHz samples; scaled to volumePercent and duplicated to both slots.
  bool write(const int16_t *samples, size_t count, uint8_t volumePercent);

 private:
  i2s_chan_handle_t channel_ = nullptr;
  bool installed_ = false;
  bool open_ = false;
};
