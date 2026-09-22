#include "ServoMotion.h"
#include "Memory.h"

bool ServoMotion::begin(uint8_t pin, uint8_t channel, uint8_t initialAngle) {
  if (ready_) return true;
  channel_ = channel;
  const float start =
      initialAngle > 180 ? 180.0f : static_cast<float>(initialAngle);
  currentAngle_ = start;
  startAngle_ = start;
  targetAngle_ = start;
  if (ledcSetup(channel_, kFrequencyHz, kResolutionBits) <= 0) return false;
  ledcAttachPin(pin, channel_);
  ready_ = true;
  moving_ = false;
  lastUpdateMs_ = millis();
  writeAngle(currentAngle_);
  commands_ = xQueueCreateStatic(1, sizeof(Command), queueStorage_, &queueControl_);
  if (!commands_ || xTaskCreatePinnedToCore(task, "servo", 2048, this, 4,
                                          nullptr, 1) != pdPASS) {
    ledcDetachPin(pin);
    ready_ = false;
    return false;
  }
  return true;
}

void ServoMotion::moveTo(float angle) {
  if (!ready_ || !isfinite(angle)) return;
  const Command command{angle, ++submitted_};
  xQueueOverwrite(commands_, &command);
}

void ServoMotion::hold() { hold_ = true; }

void ServoMotion::task(void *context) {
  auto *motion = static_cast<ServoMotion *>(context);
  TickType_t tick = xTaskGetTickCount();
  uint32_t sampled = 0;
  for (;;) {
    if (motion->hold_.exchange(false)) {
      // Forget queued targets so a revoked gesture cannot resume later.
      xQueueReset(motion->commands_);
      motion->applied_ = motion->submitted_.load();
      motion->moving_ = false;
      motion->startAngle_ = motion->currentAngle_;
      motion->targetAngle_ = motion->currentAngle_;
    }
    Command target;
    if (xQueueReceive(motion->commands_, &target, 0) == pdTRUE) {
      motion->applyTarget(target.angle);
      motion->applied_ = target.sequence;
    }
    motion->service(millis());
    if (millis() - sampled >= 5000) {
      memory::sampleTask(motion->channel_ == 2 ? "servo-turn" : "servo-tilt");
      sampled = millis();
    }
    vTaskDelayUntil(&tick, pdMS_TO_TICKS(10));
  }
}

void ServoMotion::applyTarget(float angle) {
  if (angle < 0.0f) angle = 0.0f;
  if (angle > 180.0f) angle = 180.0f;

  const float pending = angle > targetAngle_ ? angle - targetAngle_
                                             : targetAngle_ - angle;
  if (!moving_ && pending < 0.1f) return;

  startAngle_ = currentAngle_;
  targetAngle_ = angle;
  const float distance = targetAngle_ > startAngle_
                             ? targetAngle_ - startAngle_
                             : startAngle_ - targetAngle_;
  float durationMs = distance / degreesPerSecond_.load() * 1000.0f;
  if (durationMs < kMinimumMoveMs) durationMs = kMinimumMoveMs;
  moveDurationMs_ = static_cast<uint32_t>(durationMs);
  moveStartMs_ = millis();
  lastUpdateMs_ = moveStartMs_;
  moving_ = true;
}

void ServoMotion::setSpeed(float degreesPerSecond) {
  if (!isfinite(degreesPerSecond)) return;
  if (degreesPerSecond < 1.0f) degreesPerSecond = 1.0f;
  degreesPerSecond_ = degreesPerSecond;
}

void ServoMotion::service(uint32_t now) {
  if (!ready_ || !moving_ || now - lastUpdateMs_ < kUpdateIntervalMs) return;
  lastUpdateMs_ = now;

  float t = moveDurationMs_ == 0
                ? 1.0f
                : static_cast<float>(now - moveStartMs_) / moveDurationMs_;
  if (t >= 1.0f) {
    t = 1.0f;
    moving_ = false;
  }
  // Smoothstep easing: velocity is zero at both ends, so starts, stops, and
  // direction reversals are gentle instead of snapping.
  const float eased = t * t * (3.0f - 2.0f * t);
  currentAngle_ = startAngle_ + (targetAngle_ - startAngle_) * eased;
  writeAngle(currentAngle_);
}

void ServoMotion::writeAngle(float angle) {
  const float pulseUs =
      minimumPulseUs_ +
      (static_cast<float>(maximumPulseUs_ - minimumPulseUs_) * angle) / 180.0f;
  constexpr float kPeriodUs = 1000000.0f / kFrequencyHz;
  constexpr uint32_t kMaximumDuty = (1UL << kResolutionBits) - 1;
  const uint32_t duty =
      static_cast<uint32_t>(pulseUs * kMaximumDuty / kPeriodUs + 0.5f);
  ledcWrite(channel_, duty);
}
