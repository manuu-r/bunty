#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/queue.h>

// Small, non-blocking hobby-servo driver. It owns one LEDC channel and glides
// to a target angle along an eased profile so display, touch, and network work
// keep running while Bunty changes posture. Position is a pure function of
// wall-clock time, so a stalled loop() resumes the glide smoothly instead of
// snapping several degrees at once.
class ServoMotion {
 public:
  bool begin(uint8_t pin, uint8_t channel, uint8_t initialAngle);
  void moveTo(float angle);
  // Chases a target that changes every tick, such as speech loudness. A new
  // target bends the current path instead of restarting an eased glide from
  // rest, and moveTo() returns the servo to ordinary glides.
  void follow(float angle, float maxDegreesPerSecond);
  // Safe from another task; applied on the servo task's next tick.
  void hold();
  bool moving() const { return submitted_ != applied_ || moving_; }

  // Glide rate for subsequent moves. A gesture that should read as deliberate
  // wants a low value; the default suits small posture corrections.
  void setSpeed(float degreesPerSecond);
  // Set once before begin; persistent calibration is loaded by setupServos.
  void setPulseRange(uint16_t minimum, uint16_t maximum) {
    if (!ready_ && minimum >= 500 && maximum <= 2500 && minimum < maximum) {
      minimumPulseUs_ = minimum;
      maximumPulseUs_ = maximum;
    }
  }

 private:
  static void task(void *context);
  void service(uint32_t now);
  void applyTarget(float angle);
  void applyFollow(float angle, float maxDegreesPerSecond);
  void serviceFollow(uint32_t now);
  struct Command { float angle; uint32_t sequence; float followRate; };
  QueueHandle_t commands_ = nullptr;
  StaticQueue_t queueControl_ = {};
  uint8_t queueStorage_[sizeof(Command)] = {};
  std::atomic<uint32_t> submitted_{0};
  std::atomic<uint32_t> applied_{0};
  static constexpr uint32_t kFrequencyHz = 50;
  // ESP32-S3 LEDC timers top out at 14 bits. Requesting 16 bits makes
  // ledcSetup() fail and leaves the SG90 signal pin silent.
  static constexpr uint8_t kResolutionBits = 14;
  // Most SG90-class servos are specified around 1–2 ms. The wider 0.5–2.5 ms
  // range can drive an uncalibrated unit into its mechanical stops, where it
  // buzzes and jerks even though the requested angle is changing smoothly.
  static constexpr uint16_t kMinimumPulseUs = 1000;
  static constexpr uint16_t kMaximumPulseUs = 2000;
  uint16_t minimumPulseUs_ = kMinimumPulseUs;
  uint16_t maximumPulseUs_ = kMaximumPulseUs;
  // 50 Hz sub-degree position updates: each move is a continuous glide rather
  // than a stack of visible one-degree jumps.
  static constexpr uint32_t kUpdateIntervalMs = 20;
  static constexpr float kDefaultDegreesPerSecond = 20.0f;
  static constexpr float kMinimumMoveMs = 220.0f;
  static constexpr float kFollowTimeConstantMs = 60.0f;

  void writeAngle(float angle);

  uint8_t channel_ = 0;
  std::atomic<float> degreesPerSecond_{kDefaultDegreesPerSecond};
  float currentAngle_ = 90.0f;
  float startAngle_ = 90.0f;
  float targetAngle_ = 90.0f;
  uint32_t moveStartMs_ = 0;
  uint32_t moveDurationMs_ = 0;
  uint32_t lastUpdateMs_ = 0;
  std::atomic<bool> moving_{false};
  std::atomic<bool> hold_{false};
  bool following_ = false;
  float followRate_ = 0.0f;
  bool ready_ = false;
};
