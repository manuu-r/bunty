#pragma once

#include <Arduino.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>
#include <freertos/semphr.h>
#include <atomic>

enum class TapDecision {
  Confirm,
  Cancel,
  TimedOut,
};

// Keep the signal evidence with each detection. Pairing only needs the event,
// while destructive runtime gestures can additionally demand a deliberate,
// enclosure-level knock instead of accepting every microphone impulse.
struct TapEvent {
  uint32_t occurredAt = 0;
  uint32_t level = 0;
  uint32_t noiseFloor = 0;
};

// Reads the digital MEMS microphone and turns short, high-energy impulses into
// debounced tap events. The detector tracks the room's noise floor, requires
// the signal to settle before rearming, and applies a refractory period so one
// physical knock is not counted twice.
//
// It is also the microphone's only reader. Voice capture subscribes to the PCM
// this detector is already pulling rather than claiming the I2S port for
// itself, so tap gestures keep working while Bunty is listening.
class TapInput {
 public:
  bool begin();
  void end();
  bool available() const { return available_; }

  // Non-blocking; returns true once for each detected physical tap.
  bool poll(TapEvent *event = nullptr);

  // One tap confirms after the double-tap window closes. A second tap inside
  // that window cancels immediately.
  TapDecision waitForPairingDecision();

  void reset();
  void suppressFor(uint32_t milliseconds);

  bool startCapture(size_t ringBytes);
  void stopCapture();
  bool capturing() const { return capturing_; }

  // Blocks up to waitMs for microphone PCM. Returns the bytes written.
  size_t readCapture(void *destination, size_t bytes, uint32_t waitMs);

 private:
  static void detectorTask(void *context);
  bool detectTap(TapEvent *event);
  uint32_t readLevel();

  std::atomic<bool> available_{false};
  std::atomic<bool> detectorRunning_{false};
  StaticQueue_t tapQueueControl_;
  uint8_t tapQueueStorage_[8 * sizeof(TapEvent)];
  StaticSemaphore_t captureMutexControl_;
  SemaphoreHandle_t captureMutex_ = nullptr;
  bool armed_ = false;
  QueueHandle_t tapQueue_ = nullptr;
  uint32_t noiseFloor_ = 2500;
  uint32_t lastTapAt_ = 0;
  uint32_t quietSince_ = 0;
  std::atomic<uint32_t> suppressedUntil_{0};

  StreamBufferHandle_t captureStream_ = nullptr;
  uint8_t *captureStorage_ = nullptr;
  StaticStreamBuffer_t captureControl_ = {};
  std::atomic<bool> capturing_{false};
  std::atomic<uint32_t> captureDroppedBytes_{0};
  // One of the two I2S slots carries the microphone and the other is silent.
  // -1 means the active slot has not been measured yet.
  int8_t captureSlot_ = -1;
  uint64_t slotEnergy_[2] = {0, 0};
  uint16_t slotProbeReads_ = 0;
};
