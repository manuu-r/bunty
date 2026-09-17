#include "Log.h"
#include "TapGestures.h"

#include "Config.h"
#include "TapInput.h"

namespace {

using namespace cfg::tap;

// A tap qualifies only if it is loud in absolute terms *and* well clear of the
// room's current noise floor.
bool strongEnough(const TapEvent &event, uint32_t minimumLevel,
                  uint32_t noiseMultiplier) {
  const uint32_t floor = event.noiseFloor == 0 ? 1 : event.noiseFloor;
  return event.level >= minimumLevel &&
         static_cast<uint64_t>(event.level) >=
             static_cast<uint64_t>(floor) * noiseMultiplier;
}

}  // namespace

void TapGestures::reset() {
  state_ = State::Idle;
  tapCount_ = 0;
  lastTapAt_ = 0;
  armFirstTapAt_ = 0;
}

TapGestures::Intents TapGestures::serviceSleeping() {
  Intents intents;
  TapEvent event;
  if (taps_.poll(&event) &&
      strongEnough(event, kWakeMinimumLevel, kWakeNoiseMultiplier)) {
    intents.wake = true;
    intents.interacted = true;
  }
  return intents;
}

void TapGestures::expireStaleGesture(uint32_t now, Intents &intents) {
  switch (state_) {
    case State::CollectingFirstGroup:
      if (now - lastTapAt_ > kUnpairMaximumGapMs) reset();
      return;
    case State::WaitingForSecondGroup:
      if (now - lastTapAt_ > kUnpairInterGroupMaximumMs) {
        Serial.println("[unpair] second tap group timed out");
        reset();
      }
      return;
    case State::CollectingSecondGroup:
      if (now - lastTapAt_ > kUnpairMaximumGapMs) {
        Serial.println("[unpair] second tap group incomplete");
        reset();
      }
      return;
    case State::AwaitingConfirmation:
      if (now - promptedAt_ > kUnpairConfirmationTimeoutMs) {
        reset();
        intents.cancelPrompt = true;
        intents.interacted = true;
        Serial.println("[unpair] confirmation timed out");
      }
      return;
    case State::Idle:
      return;
  }
}

TapGestures::Intents TapGestures::service(bool voiceReady) {
  Intents intents;
  const uint32_t now = millis();
  expireStaleGesture(now, intents);
  if (intents.cancelPrompt) return intents;

  TapEvent event;
  if (!taps_.poll(&event)) return intents;

  if (state_ == State::AwaitingConfirmation) {
    if (static_cast<int32_t>(event.occurredAt - promptedAt_) < 0) return intents;
    const uint32_t sincePrompt = event.occurredAt - promptedAt_;
    // A visible one-second guard keeps the echoes of the gesture that opened
    // this screen from confirming it.
    if (sincePrompt < kUnpairConfirmationGuardMs ||
        sincePrompt > kUnpairConfirmationTimeoutMs ||
        !strongEnough(event, kUnpairMinimumLevel, kUnpairNoiseMultiplier)) {
      return intents;
    }
    Serial.println("[unpair] guarded confirmation accepted");
    intents.confirmUnpair = true;
    intents.interacted = true;
    return intents;
  }

  // Firm double taps are still valid voice gestures. The 3-pause-3 unpair
  // recognizer below must never steal a strong but ordinary double tap.
  if (voiceReady && strongEnough(event, kWakeMinimumLevel,
                                 kWakeNoiseMultiplier)) {
    const uint32_t gap = event.occurredAt - armFirstTapAt_;
    if (armFirstTapAt_ != 0 && gap >= kArmDoubleTapMinimumGapMs &&
        gap <= kArmDoubleTapWindowMs) {
      armFirstTapAt_ = 0;
      intents.armVoice = true;
      intents.interacted = true;
      Serial.println("[tap] double tap armed voice");
    } else {
      armFirstTapAt_ = event.occurredAt;
    }
  }

  if (!strongEnough(event, kUnpairMinimumLevel, kUnpairNoiseMultiplier)) {
    return intents;
  }
  intents.interacted = true;

  const uint32_t gap = event.occurredAt - lastTapAt_;
  const unsigned level = static_cast<unsigned>(event.level);

  switch (state_) {
    case State::Idle:
      tapCount_ = 1;
      lastTapAt_ = event.occurredAt;
      state_ = State::CollectingFirstGroup;
      logPrintf("[unpair] first group 1/%u (level %u)\n",
                    kUnpairTapsPerGroup, level);
      return intents;

    case State::CollectingFirstGroup:
      if (gap < kUnpairMinimumGapMs) return intents;
      if (gap > kUnpairMaximumGapMs) {
        tapCount_ = 1;
        lastTapAt_ = event.occurredAt;
        logPrintf("[unpair] first group restarted 1/%u (level %u)\n",
                      kUnpairTapsPerGroup, level);
        return intents;
      }
      ++tapCount_;
      lastTapAt_ = event.occurredAt;
      logPrintf("[unpair] first group %u/%u (level %u)\n", tapCount_,
                    kUnpairTapsPerGroup, level);
      if (tapCount_ < kUnpairTapsPerGroup) return intents;
      tapCount_ = 0;
      state_ = State::WaitingForSecondGroup;
      Serial.println("[unpair] first group complete; pause, then tap 3 times");
      return intents;

    case State::WaitingForSecondGroup:
      if (gap < kUnpairInterGroupMinimumMs) return intents;
      if (gap > kUnpairInterGroupMaximumMs) {
        tapCount_ = 1;
        lastTapAt_ = event.occurredAt;
        state_ = State::CollectingFirstGroup;
        Serial.println("[unpair] pause too long; new first group 1/3");
        return intents;
      }
      tapCount_ = 1;
      lastTapAt_ = event.occurredAt;
      state_ = State::CollectingSecondGroup;
      logPrintf("[unpair] second group 1/%u (level %u)\n",
                    kUnpairTapsPerGroup, level);
      return intents;

    case State::CollectingSecondGroup:
      if (gap < kUnpairMinimumGapMs) return intents;
      if (gap > kUnpairMaximumGapMs) {
        tapCount_ = 1;
        lastTapAt_ = event.occurredAt;
        state_ = State::CollectingFirstGroup;
        Serial.println("[unpair] second group restarted as first group 1/3");
        return intents;
      }
      ++tapCount_;
      lastTapAt_ = event.occurredAt;
      logPrintf("[unpair] second group %u/%u (level %u)\n", tapCount_,
                    kUnpairTapsPerGroup, level);
      if (tapCount_ < kUnpairTapsPerGroup) return intents;
      tapCount_ = 0;
      state_ = State::AwaitingConfirmation;
      promptedAt_ = millis();
      // Discard enclosure echoes already queued with the gesture; confirmation
      // needs one more firm tap after the guard.
      taps_.reset();
      taps_.suppressFor(kUnpairConfirmationGuardMs);
      intents.promptUnpair = true;
      return intents;

    case State::AwaitingConfirmation:
      return intents;
  }
  return intents;
}
