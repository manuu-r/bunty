#pragma once

#include <Arduino.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <atomic>

// The microphone pair's only reader. A task pulls both halves of every I2S
// frame continuously: one half feeds voice capture, and the pair feeds the
// direction estimator in SoundDirection.
class MicrophoneInput {
 public:
  bool begin();
  void end();
  bool available() const { return available_; }

  bool startCapture(size_t ringBytes);
  void stopCapture();
  bool capturing() const { return capturing_; }

  // Blocks up to waitMs for microphone PCM. Returns the bytes written.
  size_t readCapture(void *destination, size_t bytes, uint32_t waitMs);

 private:
  static void readerTask(void *context);
  void readFrames();

  std::atomic<bool> available_{false};
  std::atomic<bool> readerRunning_{false};
  StaticSemaphore_t captureMutexControl_;
  SemaphoreHandle_t captureMutex_ = nullptr;

  StreamBufferHandle_t captureStream_ = nullptr;
  uint8_t *captureStorage_ = nullptr;
  StaticStreamBuffer_t captureControl_ = {};
  std::atomic<bool> capturing_{false};
  std::atomic<uint32_t> captureDroppedBytes_{0};
};
