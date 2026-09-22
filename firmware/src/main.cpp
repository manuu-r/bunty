#include "Log.h"
#include <Preferences.h>
#include <WiFi.h>

#include "Animations.h"
#include "Config.h"
#include "Ui.h"
#include "Dashboard.h"
#include "Pairing.h"
#include "Power.h"
#include "Touch.h"
#include "ServoMotion.h"
#include "Memory.h"
#include "InternalAllocation.h"
#include "flow32/graphics/Display.h"

namespace {

Preferences preferences;
BuntyAnimations *animations = nullptr;
ServoMotion turnServo;
ServoMotion tiltServo;
bool turnServoReady = false;
bool tiltServoReady = false;
uint8_t turnHome = cfg::servo::kTurnHomeAngle;
uint8_t tiltHome = cfg::servo::kTiltHomeAngle;

String deviceName;
String hardwareId;

bool runtimeStarted = false;
bool setupReady = false;

// What owns the screen. Sleeping and waking are Bunty's face; the dashboard is
// everything else. Network work continues in every state.
enum class DisplayState : uint8_t { Dashboard, Sleeping, Waking };
DisplayState displayState = DisplayState::Dashboard;
uint32_t lastInteractionAt = 0;

// Bunty's eyes greet the user for a moment after pairing completes.
constexpr uint32_t kGreetingMs = 2500;
bool greetingEyesShown = false;
uint32_t greetingEndsAt = 0;

// Bunty glances left and right before swapping the dashboard page, so a page
// change reads as a deliberate gesture rather than a silent redraw.
enum class SweepPhase : uint8_t { Idle, Right, Left, Home };
SweepPhase sweepPhase = SweepPhase::Idle;
// Which servos the power budget granted for the sweep in progress.
bool sweepTurnClaimed = false;
bool sweepTiltClaimed = false;

wl_status_t lastWifiStatus = WL_NO_SHIELD;

// ------------------------------------------------------------- identity ---

void loadIdentity() {
  const uint64_t chipId = ESP.getEfuseMac();
  char name[24];
  snprintf(name, sizeof(name), "Bunty-%06llX",
           static_cast<unsigned long long>(chipId & 0xFFFFFFULL));
  deviceName = name;
  char identifier[24];
  snprintf(identifier, sizeof(identifier), "bunty-%012llx",
           static_cast<unsigned long long>(chipId & 0xFFFFFFFFFFFFULL));
  hardwareId = identifier;
}

// -------------------------------------------------------------- runtime ---

void startRuntime(bool reconnectWifi) {
  runtimeStarted = true;
  displayState = DisplayState::Dashboard;
  lastInteractionAt = millis();
  dashboard::restart();

  if (reconnectWifi) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(preferences.getString(cfg::nvs::kWifiSsid, "").c_str(),
               preferences.getString(cfg::nvs::kWifiPassword, "").c_str());
  }
  // On battery the radio sleeps between dashboard polls; power::service()
  // owns that decision on both the cold-boot and the newly-paired path.
  power::markBusy();
  lastWifiStatus = WiFi.status();
  if (!greetingEyesShown) dashboard::render();
}

void enterSleep() {
  touch::flush();
  displayState = DisplayState::Sleeping;
  {
    ui::Guard guard;
    animations->beginSleeping(millis());
  }
  ui::fadeBacklightTo(cfg::ui::kSleepBacklightDuty);
  Serial.println("[display] sleeping; background sync remains active");
}

void wake() {
  displayState = DisplayState::Waking;
  {
    ui::Guard guard;
    animations->beginWaking(millis());
  }
  ui::fadeBacklightTo(255);
  Serial.println("[display] wake touch accepted");
}

void forgetPairing() {
  ui::showUnpaired();
  dashboard::notifyGaugeBeforeUnpair();
  WiFi.disconnect(true, true);

  // Keep local unpairing scoped to the Gauge relationship.
  for (const char *key : cfg::nvs::kGaugeKeys) preferences.remove(key);
  dashboard::clear();
  Serial.println("[unpair] cleared local Gauge pairing, Wi-Fi, and cache");
  delay(800);
  ESP.restart();
}

// Returns true once the sweep has completed and the caller may redraw. Driven
// from loop() rather than blocking, so the display keeps running through
// the gesture.
bool servicePageSweep() {
  static bool preferTilt = false;
  if (!cfg::servo::kPageSweep || (!turnServoReady && !tiltServoReady)) return true;

  const bool moving = (turnServoReady && turnServo.moving()) ||
                      (tiltServoReady && tiltServo.moving());
  const auto moveBoth = [](float turnAngle, float tiltAngle) {
    if (sweepTurnClaimed) turnServo.moveTo(turnAngle);
    if (sweepTiltClaimed) tiltServo.moveTo(tiltAngle);
  };
  const auto releaseSweep = []() {
    if (sweepTurnClaimed && power::held(power::Load::kTurnServo)) {
      power::release(power::Load::kTurnServo);
    }
    if (sweepTiltClaimed && power::held(power::Load::kTiltServo)) {
      power::release(power::Load::kTiltServo);
    }
    sweepTurnClaimed = sweepTiltClaimed = false;
  };

  switch (sweepPhase) {
    case SweepPhase::Idle: {
      // The rail allows one motor. Alternate complete gestures instead of
      // always granting yaw first and starving pitch on every page.
      sweepTurnClaimed = sweepTiltClaimed = false;
      const bool useTilt = tiltServoReady && (preferTilt || !turnServoReady);
      if (useTilt) sweepTiltClaimed = power::claim(power::Load::kTiltServo);
      else if (turnServoReady) sweepTurnClaimed = power::claim(power::Load::kTurnServo);
      if (sweepTurnClaimed || sweepTiltClaimed) preferTilt = !useTilt;
      if (!sweepTurnClaimed && !sweepTiltClaimed) return true;
      turnServo.setSpeed(power::servoSpeed(cfg::servo::kSweepDegreesPerSecond));
      tiltServo.setSpeed(power::servoSpeed(cfg::servo::kSweepDegreesPerSecond));
      sweepPhase = SweepPhase::Right;
      moveBoth(turnHome + cfg::servo::kSweepDegrees,
               tiltHome + cfg::servo::kSweepDegrees);
      return false;
    }
    case SweepPhase::Right:
      if (moving) return false;
      sweepPhase = SweepPhase::Left;
      moveBoth(turnHome - cfg::servo::kSweepDegrees,
               tiltHome - cfg::servo::kSweepDegrees);
      return false;
    case SweepPhase::Left:
      if (moving) return false;
      sweepPhase = SweepPhase::Home;
      moveBoth(turnHome, tiltHome);
      return false;
    case SweepPhase::Home:
      if (moving) return false;
      releaseSweep();
      sweepPhase = SweepPhase::Idle;
      return true;
  }
  return true;
}

enum class Overlay : uint8_t { kNone, kSettings, kConfirmUnpair, kConfirmDisconnect };
Overlay overlay = Overlay::kNone;

ui::SettingsStatus settingsStatus() {
  static String network;
  static String server;
  network = preferences.getString(cfg::nvs::kWifiSsid, "");
  server = preferences.getString(cfg::nvs::kServerId, "");
  ui::SettingsStatus status;
  status.paired = preferences.getBool(cfg::nvs::kPaired, false);
  status.online = WiFi.status() == WL_CONNECTED && dashboard::online();
  status.network = network.c_str();
  status.server = server.c_str();
  status.signal = WiFi.RSSI();
  return status;
}

void openSettings() {
  overlay = Overlay::kSettings;
  touch::flush();
  ui::showSettings(settingsStatus());
}

void closeOverlay() {
  overlay = Overlay::kNone;
  touch::flush();
  dashboard::render();
}

// Drops the link without forgetting anything, so the same Mac can be rejoined
// by restarting.
void disconnectFromGauge() {
  WiFi.disconnect(false, false);
  dashboard::forgetDiscovery();
  ui::showError("Disconnected");
  delay(1200);
  closeOverlay();
}

void serviceSettingsTouch(const touch::Event &event) {
  if (event.kind != touch::Event::Kind::kTap) return;
  switch (overlay) {
    case Overlay::kSettings:
      switch (ui::settingsRowAt(event.x, event.y)) {
        case ui::SettingsRow::kPair:
          ui::showError("Restarting to pair");
          delay(900);
          forgetPairing();
          break;
        case ui::SettingsRow::kDisconnect:
          overlay = Overlay::kConfirmDisconnect;
          touch::flush();
          ui::showConfirm("DISCONNECT?", "Stays paired with this Mac");
          break;
        case ui::SettingsRow::kUnpair:
          overlay = Overlay::kConfirmUnpair;
          touch::flush();
          ui::showConfirm("UNPAIR?", "Bunty will forget this Mac");
          break;
        case ui::SettingsRow::kClose:
          closeOverlay();
          break;
        case ui::SettingsRow::kNone:
          break;
      }
      return;
    case Overlay::kConfirmUnpair:
      if (ui::confirmAcceptedAt(event.x, event.y)) forgetPairing();
      else if (ui::confirmRejectedAt(event.x, event.y)) openSettings();
      else closeOverlay();
      return;
    case Overlay::kConfirmDisconnect:
      if (ui::confirmAcceptedAt(event.x, event.y)) disconnectFromGauge();
      else if (ui::confirmRejectedAt(event.x, event.y)) openSettings();
      else closeOverlay();
      return;
    case Overlay::kNone:
      return;
  }
}

// Returns true while a face animation intentionally owns the screen.
bool serviceFace() {
  const uint32_t now = millis();
  if (displayState == DisplayState::Sleeping) {
    if (touch::poll().kind != touch::Event::Kind::kNone) {
      power::markBusy();
      wake();
      // The waking touch must not also act on the dashboard when it returns.
      touch::flush();
      return true;
    }
    ui::Guard guard;
    animations->serviceSleeping(now);
    return false;
  }
  if (displayState != DisplayState::Waking) return false;

  bool stillWaking = false;
  {
    ui::Guard guard;
    stillWaking = animations->serviceWaking(now);
  }
  if (stillWaking) return true;

  displayState = DisplayState::Dashboard;
  lastInteractionAt = now;
  dashboard::render();
  return false;
}

void serviceRuntime() {
  if (greetingEyesShown && static_cast<int32_t>(millis() - greetingEndsAt) < 0) return;
  if (greetingEyesShown) {
    greetingEyesShown = false;
    displayState = DisplayState::Dashboard;
    lastInteractionAt = millis();
    dashboard::render();
  }

  if (displayState == DisplayState::Dashboard) {
    const touch::Event event = touch::poll();
    if (event.kind != touch::Event::Kind::kNone) {
      lastInteractionAt = millis();
      power::markBusy();
    }
    if (overlay != Overlay::kNone) {
      serviceSettingsTouch(event);
      return;
    }
    if (event.kind == touch::Event::Kind::kLongPress ||
        (event.kind == touch::Event::Kind::kTap &&
         ui::settingsControlAt(event.x, event.y))) {
      openSettings();
      return;
    }
  } else if (serviceFace()) {
    return;
  }

  if (displayState == DisplayState::Dashboard && overlay == Overlay::kNone &&
      millis() - lastInteractionAt >= cfg::dashboard::kIdleSleepMs) {
    enterSleep();
  }

  const wl_status_t wifiStatus = WiFi.status();
  if (wifiStatus != lastWifiStatus) {
    lastWifiStatus = wifiStatus;
    dashboard::forgetDiscovery();
    dashboard::refreshSoon();
    if (displayState == DisplayState::Dashboard) {
      dashboard::render();
    } else {
      ui::Guard guard;
      ui::drawTopBar(true);
    }
  }

  if (dashboard::serviceFetch() && displayState == DisplayState::Dashboard) {
    dashboard::render();
  }

  if (displayState == DisplayState::Dashboard && dashboard::pageDue()) {
    // The page waits for the glance to finish; render() then restarts the page
    // timer, so the next sweep is a full interval away.
    if (servicePageSweep()) {
      dashboard::nextPage();
      dashboard::render();
    }
  }
}

// ----------------------------------------------------------------- boot ---

void runServoDemo() {
  const auto step = [](ServoMotion &motion, bool ready, float angle) {
    if (!ready) return;
    motion.moveTo(angle);
    delay(40);
    while (motion.moving()) delay(10);
    delay(120);
  };
  logPrintf("[servo] demo alternating turn/tilt\n");
  turnServo.setSpeed(cfg::servo::kBootDemoDegreesPerSecond);
  tiltServo.setSpeed(cfg::servo::kBootDemoDegreesPerSecond);
  step(turnServo, turnServoReady, turnHome);
  step(tiltServo, tiltServoReady, tiltHome);
  logPrintf("[servo] parked at home: turn %u, tilt %u\n", turnHome, tiltHome);
  for (;;) delay(1000);
}

void setupServos() {
  if (!cfg::servo::kEnabled) {
    pinMode(cfg::pin::kTurnServo, OUTPUT);
    pinMode(cfg::pin::kTiltServo, OUTPUT);
    digitalWrite(cfg::pin::kTurnServo, LOW);
    digitalWrite(cfg::pin::kTiltServo, LOW);
    logPrintf("[servo] disabled: both PWM signal pins held low\n");
    return;
  }
  Preferences calibration;
  const bool stored = calibration.begin("bunty-motion", false);
  const auto start = [&](ServoMotion &motion, bool enabled, int pin,
                        uint8_t channel, uint8_t &home, const char *label) {
    if (!enabled) {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, LOW);
      return false;
    }
    char key[16];
    snprintf(key, sizeof(key), "%s-home", label);
    if (stored) {
      const uint8_t value = calibration.getUChar(key, home);
      if (value >= cfg::servo::kSweepDegrees && value <= 180 - cfg::servo::kSweepDegrees) home = value;
      if (!calibration.isKey(key)) calibration.putUChar(key, home);
    }
    snprintf(key, sizeof(key), "%s-min", label);
    const uint16_t minimum = stored ? calibration.getUShort(key, 1000) : 1000;
    if (stored && !calibration.isKey(key)) calibration.putUShort(key, minimum);
    snprintf(key, sizeof(key), "%s-max", label);
    const uint16_t maximum = stored ? calibration.getUShort(key, 2000) : 2000;
    if (stored && !calibration.isKey(key)) calibration.putUShort(key, maximum);
    motion.setPulseRange(minimum, maximum);
    snprintf(key, sizeof(key), "%s-speed", label);
    float speed = stored ? calibration.getFloat(key, cfg::servo::kSweepDegreesPerSecond) : cfg::servo::kSweepDegreesPerSecond;
    if (!isfinite(speed) || speed < 1 || speed > 180) speed = cfg::servo::kSweepDegreesPerSecond;
    if (stored && !calibration.isKey(key)) calibration.putFloat(key, speed);
    if (!motion.begin(pin, channel, home)) {
      logPrintf("[servo] could not initialize GPIO %d %s PWM\n", pin, label);
      return false;
    }
    motion.setSpeed(speed);
    logPrintf("[servo] GPIO %d homed at %u degrees\n", pin, home);
    return true;
  };
  turnServoReady = start(turnServo, cfg::servo::kTurnEnabled,
                         cfg::pin::kTurnServo, cfg::servo::kTurnChannel,
                         turnHome, "turn");
  tiltServoReady = start(tiltServo, cfg::servo::kTiltEnabled,
                         cfg::pin::kTiltServo, cfg::servo::kTiltChannel,
                         tiltHome, "tilt");
  if (stored) calibration.end();
  if (cfg::servo::kBootDemo) runServoDemo();
}


}  // namespace

void setup() {
  memory::beginAllocationScopes();
  Serial.begin(115200);
  delay(100);

  if (!ui::begin()) return;
  setupReady = true;
  static BuntyAnimations sharedAnimations(ui::display());
  animations = &sharedAnimations;
  logPrintf("[display] free heap %u bytes, psram %u bytes\n",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getPsramSize()));

  setupServos();

  // No audio output: keep the amplifier shut down.
  pinMode(cfg::pin::kAmpShutdown, OUTPUT);
  digitalWrite(cfg::pin::kAmpShutdown, LOW);

  preferences.begin(cfg::nvs::kNamespace, false);
  power::begin(preferences);
  loadIdentity();
  dashboard::begin(preferences, hardwareId);

  const bool touchReady = touch::begin();
  pairing::begin(preferences, deviceName, hardwareId);

  if (preferences.getBool(cfg::nvs::kPaired, false)) {
    startRuntime(true);
    ui::fadeBacklight(true);
    if (!touchReady) logPrintf("[touch] unavailable; local controls disabled\n");
    return;
  }

  ui::showPairing();
  ui::fadeBacklight(true);
  if (!touchReady) {
    ui::showError("Touch controller missing");
    return;
  }
  if (!pairing::start()) ui::showError(pairing::error());
}

void loop() {
  if (!setupReady) { delay(100); return; }
  power::service();

  // Servo trajectories are serviced independently by their core-1 workers.
  const uint32_t now = millis();

  if (pairing::active()) {
    if (pairing::service()) {
      // Commissioned: greet the user, then bring the dashboard up behind it.
      {
        ui::Guard guard;
        ui::display().fillScreen(cfg::ui::kBackground);
        ui::drawTopBar(false);
        animations->beginSpeaking(millis(), false);
        ui::display().present();
      }
      greetingEyesShown = true;
      greetingEndsAt = millis() + kGreetingMs;
      startRuntime(false);
    }
  } else if (runtimeStarted) {
    serviceRuntime();
  }

  if (greetingEyesShown && static_cast<int32_t>(now - greetingEndsAt) < 0) {
    ui::Guard guard;
    animations->serviceSpeaking(now);
  }
  delay(8);
}
