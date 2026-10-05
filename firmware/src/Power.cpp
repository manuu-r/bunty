#include "Log.h"
#include "Power.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <atomic>

#include "Config.h"

namespace power {
namespace {

using namespace cfg::power;

Preferences *storage_ = nullptr;
Mode mode_ = Mode::kNormal;
bool brownout_ = false;
uint32_t startedAt_ = 0;

// Guarded, not merely atomic: a claim must test and commit as one step or two
// loads both believe they fit.
portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
bool claimed_[static_cast<size_t>(Load::kCount)] = {};

bool txPowerApplied_ = false;

uint32_t lastBusyAt_ = 0;
bool idle_ = false;
bool radioSleeping_ = false;
std::atomic<bool> radioAwake_{false};

uint32_t usedMilliampSeconds_ = 0;
uint32_t lastAccountedAt_ = 0;
uint32_t lastPersistedAt_ = 0;
constexpr uint32_t kCapacityMilliampSeconds =
    static_cast<uint32_t>(kBatteryMilliampHours) * 3600u;

uint16_t loadMilliamps(Load load) {
  switch (load) {
    case Load::kTurnServo:
    case Load::kTiltServo: return kServoMilliamps;
    case Load::kAudio: return kAudioMilliamps;
    case Load::kDance: return kDanceMilliamps;
    case Load::kCount: break;
  }
  return 0;
}

uint16_t committedLocked() {
  uint16_t total = kBaseMilliamps + kBacklightMilliamps;
  for (size_t i = 0; i < static_cast<size_t>(Load::kCount); ++i) {
    if (claimed_[i]) total += loadMilliamps(static_cast<Load>(i));
  }
  return total;
}

void setRadioSleep(bool sleep) {
  if (radioSleeping_ == sleep) return;
  radioSleeping_ = sleep;
  WiFi.setSleep(sleep);
}

void setIdle(bool idle) {
  if (idle_ == idle) return;
  idle_ = idle;
  setCpuFrequencyMhz(idle ? kIdleCpuMhz : kActiveCpuMhz);
}

void accountFuel(uint32_t now) {
  const uint32_t elapsed = now - lastAccountedAt_;
  if (elapsed < 1000) return;
  lastAccountedAt_ = now;
  usedMilliampSeconds_ += committedMilliamps() * (elapsed / 1000);

  if (storage_ && now - lastPersistedAt_ >= kFuelPersistMs) {
    lastPersistedAt_ = now;
    storage_->putUInt(cfg::nvs::kFuelUsed, usedMilliampSeconds_);
  }
}

}  // namespace

void begin(Preferences &storage) {
  storage_ = &storage;
  startedAt_ = millis();
  lastBusyAt_ = startedAt_;
  lastAccountedAt_ = startedAt_;
  lastPersistedAt_ = startedAt_;
  usedMilliampSeconds_ = storage.getUInt(cfg::nvs::kFuelUsed, 0);

  brownout_ = esp_reset_reason() == ESP_RST_BROWNOUT;
  if (brownout_) {
    const uint32_t count = storage.getUInt(cfg::nvs::kBrownouts, 0) + 1;
    storage.putUInt(cfg::nvs::kBrownouts, count);
    mode_ = Mode::kConservative;
    logPrintf(
        "[power] recovered from a brown-out (%u so far); running gently for "
        "%u minutes\n",
        static_cast<unsigned>(count),
        static_cast<unsigned>(kConservativeMs / 60000));
  }

  setCpuFrequencyMhz(kIdleCpuMhz);
  idle_ = true;
  logPrintf("[power] rail %u mA, reserved %u mA, base %u mA\n",
            static_cast<unsigned>(kRailMilliamps),
            static_cast<unsigned>(kReserveMilliamps),
            static_cast<unsigned>(kBaseMilliamps));
}

bool claim(Load load) {
  if (load == Load::kCount) return false;
  if (mode_ == Mode::kSaver && load != Load::kAudio) return false;
  const size_t index = static_cast<size_t>(load);
  const uint16_t headroom = kRailMilliamps - kReserveMilliamps;

  bool granted = false;

  taskENTER_CRITICAL(&lock_);
  if (claimed_[index]) {
    granted = true;  // already ours; claiming twice is not an error
  } else if (committedLocked() + loadMilliamps(load) <= headroom) {
    claimed_[index] = true;
    granted = true;
  }
  taskEXIT_CRITICAL(&lock_);

  if (!granted) {
    logPrintf("[power] refused a %u mA load; %u mA already committed\n",
              static_cast<unsigned>(loadMilliamps(load)),
              static_cast<unsigned>(committedMilliamps()));
    return false;
  }

  markBusy();
  return true;
}

void release(Load load) {
  if (load == Load::kCount) return;
  taskENTER_CRITICAL(&lock_);
  claimed_[static_cast<size_t>(load)] = false;
  taskEXIT_CRITICAL(&lock_);
  markBusy();
}

bool held(Load load) {
  if (load == Load::kCount) return false;
  return claimed_[static_cast<size_t>(load)];
}

uint16_t committedMilliamps() {
  taskENTER_CRITICAL(&lock_);
  const uint16_t total = committedLocked();
  taskEXIT_CRITICAL(&lock_);
  return total;
}

void markBusy() {
  lastBusyAt_ = millis();
  setIdle(false);
}

void setRadioAwake(bool awake) {
  if (radioAwake_.exchange(awake) == awake) return;
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setTxPower(static_cast<wifi_power_t>(awake ? kVoiceTxPowerQuarterDbm
                                                     : kWifiTxPowerQuarterDbm));
  }
}

void service() {
  const uint32_t now = millis();
  accountFuel(now);

  bool busy = false;
  for (size_t i = 0; i < static_cast<size_t>(Load::kCount); ++i) busy |= claimed_[i];
  setIdle(!busy && now - lastBusyAt_ >= kIdleAfterMs);
  setRadioSleep(!radioAwake_);
  if (!txPowerApplied_ && WiFi.status() == WL_CONNECTED) {
    txPowerApplied_ = true;
    WiFi.setTxPower(static_cast<wifi_power_t>(kWifiTxPowerQuarterDbm));
    logPrintf("[power] Wi-Fi transmit capped at %d dBm\n",
              kWifiTxPowerQuarterDbm / 4);
  }

  if (mode_ == Mode::kConservative && now - startedAt_ >= kConservativeMs) {
    mode_ = Mode::kNormal;
    Serial.println("[power] no further brown-outs; back to full power");
  }
  if (kSaverPercent > 0 && mode_ == Mode::kNormal &&
      estimatedPercentRemaining() <= kSaverPercent) {
    mode_ = Mode::kSaver;
    Serial.println("[power] modelled charge is low; shedding load");
  }
}

uint8_t backlightCeiling() {
  switch (mode_) {
    case Mode::kConservative: return kConservativeBacklight;
    case Mode::kSaver: return kSaverBacklight;
    case Mode::kNormal: break;
  }
  return 255;
}

float servoSpeed(float requestedDegreesPerSecond) {
  if (mode_ == Mode::kNormal) return requestedDegreesPerSecond;
  // A slower glide spends less of the move at full current.
  return requestedDegreesPerSecond < kConservativeServoDegreesPerSecond
             ? requestedDegreesPerSecond
             : kConservativeServoDegreesPerSecond;
}

Mode mode() { return mode_; }

const char *modeName() {
  switch (mode_) {
    case Mode::kConservative: return "conservative";
    case Mode::kSaver: return "saver";
    case Mode::kNormal: break;
  }
  return "normal";
}

bool recoveredFromBrownout() { return brownout_; }

uint8_t estimatedPercentRemaining() {
  if (usedMilliampSeconds_ >= kCapacityMilliampSeconds) return 0;
  const uint32_t left = kCapacityMilliampSeconds - usedMilliampSeconds_;
  return static_cast<uint8_t>(left * 100u / kCapacityMilliampSeconds);
}

void resetFuelGauge() {
  usedMilliampSeconds_ = 0;
  lastAccountedAt_ = millis();
  if (storage_) storage_->putUInt(cfg::nvs::kFuelUsed, 0);
  if (mode_ == Mode::kSaver) mode_ = Mode::kNormal;
  Serial.println("[power] fuel gauge reset");
}

}  // namespace power
