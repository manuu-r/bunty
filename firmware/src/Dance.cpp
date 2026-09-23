#include "Dance.h"

#include <math.h>

#include "Power.h"

namespace {

using namespace cfg::dance;

float blend(float current, float goal) { return current + (goal - current) * kStyleBlend; }

// A sine that squares off as `snap` rises: 0 glides, 1 jumps and holds.
float wave(float phase, float snap) {
  const float sine = sinf(phase);
  const float square = sine >= 0.0f ? 1.0f : -1.0f;
  return sine + (square - sine) * snap;
}

}  // namespace

void Dance::attach(ServoMotion *turn, uint8_t turnHome, ServoMotion *tilt,
                   uint8_t tiltHome) {
  turn_ = turn;
  tilt_ = tilt;
  turnHome_ = turnHome;
  tiltHome_ = tiltHome;
}

void Dance::aim(float turnOffset, float tiltOffset, float degreesPerSecond) {
  const float rate = power::servoSpeed(degreesPerSecond);
  if (turn_) turn_->follow(turnHome_ + turnOffset, rate);
  if (tilt_) tilt_->follow(tiltHome_ + tiltOffset, rate);
}

void Dance::service(Voice::Phase phase, Emotion emotion, uint32_t now) {
  if (!turn_ && !tilt_) return;
  if (now - lastTickAt_ < kTickMs) return;
  const float dt = lastTickAt_ == 0 ? kTickMs / 1000.0f : (now - lastTickAt_) / 1000.0f;
  lastTickAt_ = now;

  if (phase == Voice::Phase::Off) {
    if (!claimed_) return;
    if (!homing_) {
      homing_ = true;
      homingSince_ = now;
      aim(0.0f, 0.0f, kPoseDegreesPerSecond);
    }
    const bool settled = !(turn_ && turn_->moving()) && !(tilt_ && tilt_->moving());
    if ((settled && now - homingSince_ > 100) || now - homingSince_ > 3000) {
      power::release(power::Load::kDance);
      claimed_ = homing_ = false;
      style_ = kStyles[0];
    }
    return;
  }

  homing_ = false;
  if (!claimed_) {
    if (!power::claim(power::Load::kDance)) return;
    claimed_ = true;
  }

  switch (phase) {
    case Voice::Phase::Connecting:
    case Voice::Phase::Listening:
      aim(0.0f, kListenTiltDegrees, kPoseDegreesPerSecond);
      return;
    case Voice::Phase::Thinking:
    case Voice::Phase::Finishing:
      aim(kThinkTurnDegrees, kThinkTiltDegrees, kPoseDegreesPerSecond);
      return;
    case Voice::Phase::Speaking:
      break;
    case Voice::Phase::Off:
      return;
  }

  // Glide between emotions rather than switching gesture in one jump. The
  // phases keep running, so a new tempo picks up where the old one was.
  const Style &goal = kStyles[static_cast<uint8_t>(emotion)];
  style_.yawAmplitude = blend(style_.yawAmplitude, goal.yawAmplitude);
  style_.yawHz = blend(style_.yawHz, goal.yawHz);
  style_.tiltAmplitude = blend(style_.tiltAmplitude, goal.tiltAmplitude);
  style_.tiltHz = blend(style_.tiltHz, goal.tiltHz);
  style_.tilt = blend(style_.tilt, goal.tilt);
  style_.yaw = blend(style_.yaw, goal.yaw);
  style_.rate = blend(style_.rate, goal.rate);
  style_.snap = blend(style_.snap, goal.snap);

  yawPhase_ = fmodf(yawPhase_ + 2.0f * PI * style_.yawHz * dt, 2.0f * PI);
  tiltPhase_ = fmodf(tiltPhase_ + 2.0f * PI * style_.tiltHz * dt, 2.0f * PI);
  aim(style_.yaw + style_.yawAmplitude * wave(yawPhase_, style_.snap),
      style_.tilt + style_.tiltAmplitude * wave(tiltPhase_, style_.snap),
      style_.rate);
}
