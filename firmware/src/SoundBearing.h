#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "GccPhat.h"

namespace hearing {

struct Bearing {
  // Negative is to Bunty's left, positive to its right. Two microphones cannot
  // tell front from back, so a source behind mirrors onto the same bearing.
  float degrees = 0.0f;
  uint8_t confidence = 0;
  uint32_t at = 0;
};

struct TrackerParams {
  float sampleRate = 48000.0f;
  float spacingMm = 60.0f;
  float speedOfSoundMmPerMs = 343.0f;
  uint32_t minRms = 500;           // both channels, after the capture gain
  float maxLevelRatio = 4.0f;      // a covered or dead capsule is not a bearing
  float minCoherence = 0.12f;
  float agreeDegrees = 15.0f;
  size_t minAgreeing = 3;
  uint32_t freshMs = 1500;
  bool swapChannels = false;       // flip if Bunty turns away from the talker
};

// Turns raw lag estimates into a bearing that is stable enough to steer by:
// gate on level and coherence, convert lag to angle, then accept a value only
// when several recent windows agree on it.
class BearingTracker {
 public:
  static constexpr size_t kHistory = 5;

  explicit BearingTracker(const TrackerParams &params = {}) : params_(params) {}
  void reset() { count_ = 0; published_ = Bearing{}; }

  // Longest delay physically possible between the capsules, in samples.
  float maxLagSamples() const {
    return params_.spacingMm / params_.speedOfSoundMmPerMs * params_.sampleRate / 1000.0f;
  }

  // True when this window produced or refreshed a published bearing.
  bool add(const GccPhat::Result &window, uint32_t now) {
    if (!window.valid || window.leftRms < params_.minRms || window.rightRms < params_.minRms)
      return false;
    const float hi = float(std::max(window.leftRms, window.rightRms));
    const float lo = float(std::min(window.leftRms, window.rightRms));
    if (hi > params_.maxLevelRatio * lo || window.coherence < params_.minCoherence) return false;
    const float limit = maxLagSamples();
    // Rejected, not clamped: a reflection must not read as a hard-sided talker.
    if (std::fabs(window.lagSamples) > limit + 0.5f) return false;
    const float sine = std::max(-1.0f, std::min(1.0f, window.lagSamples / limit));
    float degrees = -std::asin(sine) * 57.2957795f;
    if (params_.swapChannels) degrees = -degrees;

    // Forget stale windows, then keep the newest few.
    size_t kept = 0;
    for (size_t i = 0; i < count_; ++i)
      if (int32_t(now - history_[i].at) < int32_t(params_.freshMs)) history_[kept++] = history_[i];
    count_ = kept;
    if (count_ == kHistory) {
      for (size_t i = 1; i < count_; ++i) history_[i - 1] = history_[i];
      --count_;
    }
    history_[count_++] = {degrees, window.coherence, now};
    if (count_ < params_.minAgreeing) return false;

    float sorted[kHistory];
    for (size_t i = 0; i < count_; ++i) sorted[i] = history_[i].degrees;
    std::sort(sorted, sorted + count_);
    const float median = sorted[count_ / 2];
    float sum = 0, coherence = 0;
    size_t agreeing = 0;
    for (size_t i = 0; i < count_; ++i) {
      if (std::fabs(history_[i].degrees - median) > params_.agreeDegrees) continue;
      sum += history_[i].degrees;
      coherence += history_[i].coherence;
      ++agreeing;
    }
    if (agreeing < params_.minAgreeing) return false;
    published_.degrees = sum / float(agreeing);
    published_.confidence = uint8_t(std::min(100.0f, 100.0f * coherence / float(agreeing)));
    published_.at = now;
    return true;
  }

  bool bearing(Bearing *out, uint32_t now) const {
    if (published_.at == 0 || int32_t(now - published_.at) > int32_t(params_.freshMs)) return false;
    if (out) *out = published_;
    return true;
  }

 private:
  struct Entry { float degrees, coherence; uint32_t at; };
  TrackerParams params_;
  Entry history_[kHistory] = {};
  size_t count_ = 0;
  Bearing published_;
};

struct LookParams {
  float deadzoneDegrees = 10.0f;   // a head that twitches reads as nervous
  float maxTurnDegrees = 18.0f;    // the speech-dance power reservation
  uint32_t cooldownMs = 800;
  // Microphones that ride on the turning head hear bearings relative to it, so
  // each correction is added to where the head already points.
  bool micsOnHead = false;
};

// Decides where the head should look. It only moves the target when the new
// bearing is clearly elsewhere and the last move is not recent; the servo's
// own follow filter then makes the motion smooth.
class LookAt {
 public:
  explicit LookAt(const LookParams &params = {}) : params_(params) {}
  void reset() { target_ = 0; moved_ = false; }
  float target() const { return target_; }
  // True when the target changed; the caller must then drop old bearings if
  // the microphones turn with the head.
  bool update(bool valid, float degrees, uint32_t now) {
    if (!valid || !std::isfinite(degrees)) return false;
    float wanted = params_.micsOnHead ? target_ + degrees : degrees;
    wanted = std::max(-params_.maxTurnDegrees, std::min(params_.maxTurnDegrees, wanted));
    if (std::fabs(wanted - target_) < params_.deadzoneDegrees) return false;
    if (moved_ && now - movedAt_ < params_.cooldownMs) return false;
    target_ = wanted;
    movedAt_ = now;
    moved_ = true;
    return true;
  }
 private:
  LookParams params_;
  float target_ = 0;
  uint32_t movedAt_ = 0;
  bool moved_ = false;
};

}  // namespace hearing
