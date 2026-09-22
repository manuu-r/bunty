#include "Dance.h"

#include <math.h>

#include "Config.h"
#include "Power.h"

namespace {

using namespace cfg::dance;

float clamp01(float value) { return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value); }

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

void Dance::service(Voice::Phase phase, float level, uint32_t now) {
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
      fast_ = slow_ = accent_ = 0.0f;
    }
    return;
  }

  homing_ = false;
  if (!claimed_) {
    if (!power::claim(power::Load::kDance)) return;
    claimed_ = true;
  }

  // A fast envelope for syllables and a slow one for phrases; their gap is
  // the onset that kicks a nod.
  const float target = level;
  fast_ += (target - fast_) * (target > fast_ ? 0.6f : 0.15f);
  slow_ += (target - slow_) * 0.035f;

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

  const float loud = clamp01(slow_ * 1.4f);
  const float onset = clamp01((fast_ - slow_ * 1.1f) * 2.5f);

  // Strong onsets flick the yaw the other way, like a beat accent.
  if (onset > 0.45f && onsetArmed_ && now - lastAccentAt_ > 250) {
    accentSign_ = -accentSign_;
    accent_ = accentSign_ * kAccentDegrees;
    lastAccentAt_ = now;
    onsetArmed_ = false;
  } else if (onset < 0.2f) {
    onsetArmed_ = true;
  }
  accent_ *= 0.88f;

  sweepPhase_ += 2.0f * PI * kSweepHz * (0.25f + 0.75f * loud) * dt;
  if (sweepPhase_ > 2.0f * PI) sweepPhase_ -= 2.0f * PI;

  const float sweep = kSweepDegrees * (0.3f + 0.7f * loud) * sinf(sweepPhase_);
  const float tilt = kLiftDegrees * loud + kNodDegrees * onset;
  aim(sweep + accent_, tilt, kMaxDegreesPerSecond);
}
