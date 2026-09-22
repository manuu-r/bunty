#include "Log.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <time.h>

#include "Audio.h"
#include "Animations.h"
#include "Config.h"
#include "Ui.h"
#include "Voice.h"
#include "Dashboard.h"
#include "IotGatewayCredentials.h"
#include "Pairing.h"
#include "Power.h"
#include "Touch.h"
#include "SoundDirection.h"
#include "ServoMotion.h"
#include "Memory.h"
#include "InternalAllocation.h"
#include "MicrophoneInput.h"
#include "flow32/graphics/Display.h"

namespace {

Preferences preferences;
MicrophoneInput microphoneInput;
BuntyVoice buntyVoice;
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
bool voiceStarted = false;
uint32_t nextVoiceStartAt = 0;

// What owns the screen. Sleeping and waking are Bunty's face; the dashboard is
// everything else. Network work continues in every state.
enum class DisplayState : uint8_t { Dashboard, Sleeping, Waking };
DisplayState displayState = DisplayState::Dashboard;
uint32_t lastInteractionAt = 0;

bool greetingEyesShown = false;
uint32_t lastGreetingBarAt = 0;

// Voice face.
bool voiceFaceActive = false;
uint32_t voiceFaceSince = 0;
uint32_t lastMouthFrameAt = 0;
uint32_t lastWaveformAt = 0;
uint32_t lastHeartbeatAt = 0;
bool wakeIconShown = false;
bool waveformShown = false;
uint8_t waveformLevels[cfg::voice::kWaveformSamples] = {0};
size_t waveformHead = 0;

// Clock and DNS, both prerequisites for the voice gateway's TLS session.
bool publicDnsConfigured = false;
bool clockSynchronized = false;
bool clockSyncRequested = false;
uint32_t nextClockSyncAt = 0;

// Bunty glances left and right before swapping the dashboard page, so a page
// change reads as a deliberate gesture rather than a silent redraw.
enum class SweepPhase : uint8_t { Idle, Right, Left, Home };
SweepPhase sweepPhase = SweepPhase::Idle;
// Which servos the power budget granted for the sweep in progress.
bool sweepTurnClaimed = false;
bool lookClaimed = false;
uint32_t lastLookAt = 0;
float lookAngle = 0.0f;
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

bool isIotGatewayToken(const char *token) {
  if (strlen(token) != 47 || strncmp(token, "iot_", 4) != 0) return false;
  for (size_t i = 4; i < 47; ++i) {
    const char ch = token[i];
    if (!isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') {
      return false;
    }
  }
  return true;
}

// A 64-bit random hex string identifying this conversation. It is minted when a
// new gateway credential is stored -- i.e. on every provisioned flash -- and
// persists across power cycles, so the gateway keeps short-term memory within
// one firmware build but starts clean after a reflash.
void mintVoiceSession() {
  char sessionId[17];
  snprintf(sessionId, sizeof(sessionId), "%08x%08x",
           static_cast<unsigned>(esp_random()),
           static_cast<unsigned>(esp_random()));
  preferences.putString(cfg::nvs::kIotSession, sessionId);
  logPrintf("[iot-gateway] new conversation session %s\n", sessionId);
}

void syncIotGatewayCredential() {
  constexpr char compiledToken[] = IOT_GATEWAY_TOKEN;
  constexpr char compiledDevice[] = IOT_GATEWAY_DEVICE_ID;
  if (!isIotGatewayToken(compiledToken) ||
      strcmp(compiledDevice, "bunty") != 0) {
    Serial.println("[iot-gateway] no valid credential embedded in this build");
    return;
  }
  if (preferences.getString(cfg::nvs::kIotToken, "") == compiledToken &&
      preferences.getString(cfg::nvs::kIotDevice, "") == compiledDevice) {
    Serial.println("[iot-gateway] credential already stored");
    return;
  }
  const bool saved =
      preferences.putString(cfg::nvs::kIotToken, compiledToken) > 0 &&
      preferences.putString(cfg::nvs::kIotDevice, compiledDevice) > 0;
  Serial.println(saved ? "[iot-gateway] stored flashed credential"
                       : "[iot-gateway] could not store flashed credential");
  if (saved) mintVoiceSession();
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
  // On battery the radio sleeps between dashboard polls and wakes for voice;
  // power::service() owns that decision on both the cold-boot and the
  // newly-paired path.
  power::markBusy();
  lastWifiStatus = WiFi.status();
  nextVoiceStartAt = 0;
  if (!greeting::playing()) dashboard::render();
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

  // Keep local unpairing scoped to the Gauge relationship. The IoT gateway
  // credential is provisioned with the firmware and must not be discarded
  // merely because the user pairs Bunty with a different Mac.
  for (const char *key : cfg::nvs::kGaugeKeys) preferences.remove(key);
  dashboard::clear();
  Serial.println("[unpair] cleared local Gauge pairing, Wi-Fi, and cache");
  delay(800);
  ESP.restart();
}

// Returns true once the sweep has completed and the caller may redraw. Driven
// from loop() rather than blocking, so display and voice keep running through
// the gesture.
bool servicePageSweep() {
  if (!turnServoReady && !tiltServoReady) return true;

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

  // Audio can take the rail back mid-gesture; the servo is already stopped.
  if ((sweepTurnClaimed && !power::held(power::Load::kTurnServo)) ||
      (sweepTiltClaimed && !power::held(power::Load::kTiltServo))) {
    releaseSweep();
    sweepPhase = SweepPhase::Idle;
    return true;
  }

  switch (sweepPhase) {
    case SweepPhase::Idle:
      if (power::audioBusy()) return true;
      sweepTurnClaimed =
          turnServoReady && power::claim(power::Load::kTurnServo);
      sweepTiltClaimed =
          tiltServoReady && power::claim(power::Load::kTiltServo);
      if (!sweepTurnClaimed && !sweepTiltClaimed) return true;
      turnServo.setSpeed(power::servoSpeed(cfg::servo::kSweepDegreesPerSecond));
      tiltServo.setSpeed(power::servoSpeed(cfg::servo::kSweepDegreesPerSecond));
      sweepPhase = SweepPhase::Right;
      moveBoth(turnHome + cfg::servo::kSweepDegrees,
               tiltHome + cfg::servo::kSweepDegrees);
      return false;
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

void serviceLookAtSpeaker() {
  if (!turnServoReady) return;

  if (lookClaimed && !turnServo.moving()) {
    power::release(power::Load::kTurnServo);
    lookClaimed = false;
  }

  // Face forward again so the next conversation does not start from a stale bearing.
  if (!buntyVoice.armed() && !buntyVoice.engaged()) {
    if (lookAngle != 0.0f && !lookClaimed && sweepPhase == SweepPhase::Idle &&
        !power::audioBusy() && power::claim(power::Load::kTurnServo)) {
      lookClaimed = true;
      lookAngle = 0.0f;
      hearing::reset();
      turnServo.setSpeed(power::servoSpeed(cfg::hearing::kTurnDegreesPerSecond));
      turnServo.moveTo(turnHome);
    }
    return;
  }
  if (lookClaimed || sweepPhase != SweepPhase::Idle) return;
  if (!buntyVoice.armed() && !buntyVoice.engaged()) return;
  if (power::audioBusy()) return;

  hearing::Bearing heard;
  if (!hearing::bearing(&heard)) return;

  const uint32_t now = millis();
  if (now - lastLookAt < cfg::hearing::kTurnCooldownMs) return;

  float target = heard.degrees;
  if (target > cfg::hearing::kMaximumTurnDegrees) target = cfg::hearing::kMaximumTurnDegrees;
  if (target < -cfg::hearing::kMaximumTurnDegrees) target = -cfg::hearing::kMaximumTurnDegrees;
  if (fabsf(target - lookAngle) < cfg::hearing::kDeadzoneDegrees) return;

  if (!power::claim(power::Load::kTurnServo)) return;
  lookClaimed = true;
  lastLookAt = now;
  lookAngle = target;
  turnServo.setSpeed(power::servoSpeed(cfg::hearing::kTurnDegreesPerSecond));
  turnServo.moveTo(turnHome + target);
  logPrintf("[hearing] talker %+.0f deg, confidence %u\n",
            static_cast<double>(target), static_cast<unsigned>(heard.confidence));
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
      // The waking touch must not also arm voice when the dashboard returns.
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
  if (greeting::playing()) return;
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
    if (event.kind != touch::Event::Kind::kNone) {
      ui::drawTouchMarker(event.x, event.y);
    }
    if (event.kind == touch::Event::Kind::kLongPress ||
        (event.kind == touch::Event::Kind::kTap &&
         ui::settingsControlAt(event.x, event.y))) {
      openSettings();
      return;
    }
    if (event.kind == touch::Event::Kind::kTap && ui::voiceControlAt(event.x, event.y) && voiceStarted) buntyVoice.arm();
  } else if (serviceFace()) {
    return;
  }

  if (displayState == DisplayState::Dashboard && overlay == Overlay::kNone &&
      millis() - lastInteractionAt >= cfg::dashboard::kIdleSleepMs) {
    enterSleep();
  }

  // While a turn is live, or voice is armed and showing Bunty's face, keep
  // dashboard discovery, TCP timeouts, and page rendering off the screen and
  // out of the latency-sensitive capture/playback path.
  if (buntyVoice.engaged() || buntyVoice.armed()) return;

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

// ---------------------------------------------------------------- voice ---

void serviceClockSync() {
  if (WiFi.status() != WL_CONNECTED) {
    publicDnsConfigured = false;
    return;
  }

  if (!publicDnsConfigured) {
    esp_netif_t *station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
    if (station &&
        esp_netif_set_dns_info(station, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
      esp_netif_dns_info_t backup = {};
      backup.ip.type = ESP_IPADDR_TYPE_V4;
      backup.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
      esp_netif_set_dns_info(station, ESP_NETIF_DNS_BACKUP, &backup);
      publicDnsConfigured = true;
      Serial.println("[network] DNS set to 1.1.1.1 (backup 8.8.8.8)");
    } else {
      Serial.println("[network] could not apply DNS fallback");
    }
  }

  if (time(nullptr) >= cfg::net::kMinimumTrustedUnixTime) {
    if (!clockSynchronized) {
      clockSynchronized = true;
      logPrintf("[time] synchronized (Unix %lld)\n",
                    static_cast<long long>(time(nullptr)));
    }
    return;
  }
  if (clockSyncRequested &&
      static_cast<int32_t>(millis() - nextClockSyncAt) < 0) {
    return;
  }
  // TLS certificate validity depends on a real clock. This is asynchronous;
  // voice waits for it rather than blocking the display or touch input.
  configTime(0, 0, "time.cloudflare.com", "time.google.com", "pool.ntp.org");
  clockSyncRequested = true;
  nextClockSyncAt = millis() + cfg::net::kClockSyncRetryMs;
  Serial.println("[time] requesting NTP synchronization for TLS");
}

// Bunty's face while it answers: the speaking eyes plus a vector mouth whose
// opening follows the audio actually reaching the amplifier.
void serviceVoiceFace() {
  const uint32_t now = millis();
  // Settings owns the screen while it is open; the face would paint over it.
  const bool onDashboard =
      displayState == DisplayState::Dashboard && overlay == Overlay::kNone;
  const bool speaking = buntyVoice.speaking() && onDashboard;
  // Show Bunty's face the moment a touch arms voice, so the touch has visible
  // feedback; the mouth only moves once Bunty is actually talking.
  const bool attentive = speaking || (buntyVoice.armed() && onDashboard);

  if (attentive && !voiceFaceActive) {
    voiceFaceActive = true;
    voiceFaceSince = now;
    wakeIconShown = false;
    waveformShown = false;
    waveformHead = 0;
    memset(waveformLevels, 0, sizeof(waveformLevels));
    ui::Guard guard;
    ui::display().clear(cfg::ui::kBackground);
    animations->beginSpeaking(now);
    ui::drawTopBar(true);
  } else if (!attentive && voiceFaceActive) {
    if (now - voiceFaceSince < cfg::voice::kFaceMinimumMs) return;
    voiceFaceActive = false;
    // The dashboard owns the screen again once the window closes -- unless it
    // has meanwhile handed the panel to a sleep or wake animation.
    if (displayState == DisplayState::Dashboard) dashboard::render();
    return;
  }
  if (!voiceFaceActive) return;

  {
    ui::Guard guard;
    animations->serviceSpeaking(now);
  }

  // Wake icon: shown while armed and waiting, hidden once Bunty is talking.
  const bool wantWakeIcon = buntyVoice.armed() && !buntyVoice.speaking();
  if (wantWakeIcon != wakeIconShown) {
    wakeIconShown = wantWakeIcon;
    ui::Guard guard;
    if (wantWakeIcon) {
      animations->drawWakeIcon();
    } else {
      animations->clearWakeIcon();
    }
  }

  // The waveform runs for the whole armed window, not just while the VAD gate
  // is open, so "I am listening" stays visible through a pause instead of the
  // strip vanishing every time you stop to think.
  const BuntyVoice::State state = buntyVoice.state();
  const bool listening =
      buntyVoice.armed() && (state == BuntyVoice::State::Listening ||
                             state == BuntyVoice::State::Connecting ||
                             state == BuntyVoice::State::Uploading);
  if (listening) {
    if (now - lastWaveformAt >= cfg::voice::kWaveformFrameMs) {
      lastWaveformAt = now;
      uint32_t scaled =
          buntyVoice.inputLevel() * 100u / cfg::voice::kWaveformFullScaleRms;
      if (scaled > 100) scaled = 100;
      waveformLevels[waveformHead] = static_cast<uint8_t>(scaled);
      waveformHead = (waveformHead + 1) % cfg::voice::kWaveformSamples;
      waveformShown = true;
      ui::Guard guard;
      animations->drawInputWaveform(waveformLevels,
                                    cfg::voice::kWaveformSamples, waveformHead);
    }
  } else if (waveformShown) {
    waveformShown = false;
    memset(waveformLevels, 0, sizeof(waveformLevels));
    waveformHead = 0;
    ui::Guard guard;
    animations->clearInputWaveform();
  }

  // The eyes run at their own cadence; the mouth is throttled separately so a
  // 240 px band is not pushed over SPI on every loop iteration.
  if (now - lastMouthFrameAt < cfg::voice::kMouthFrameMs) return;
  lastMouthFrameAt = now;
  ui::Guard guard;
  animations->drawSpeakingMouth(
      buntyVoice.speaking() ? buntyVoice.speechLevel() : 0);
}

void logVoiceHeartbeat(uint32_t now) {
  // A periodic line so voice status is visible without catching boot output.
  // The uptime makes a reboot loop obvious: the counter restarts instead of
  // climbing.
  if (lastHeartbeatAt != 0 && now - lastHeartbeatAt < cfg::voice::kHeartbeatMs) {
    return;
  }
  lastHeartbeatAt = now;
  memory::sampleTask("loop");
  memory::report();
  logPrintf(
      "[voice] hb up=%lus runtime=%d greeting=%d wifi=%d started=%d state=%s "
      "armed=%d speech=%d rms=%u/%u noise=%u heap=%u psram=%u\n",
      static_cast<unsigned long>(now / 1000), runtimeStarted ? 1 : 0,
      greeting::playing() ? 1 : 0, WiFi.status() == WL_CONNECTED ? 1 : 0,
      voiceStarted ? 1 : 0, BuntyVoice::stateName(buntyVoice.state()),
      buntyVoice.armed() ? 1 : 0,
      buntyVoice.inputSpeech() ? 1 : 0,
      static_cast<unsigned>(buntyVoice.inputLevel()),
      static_cast<unsigned>(buntyVoice.inputThreshold()),
      static_cast<unsigned>(buntyVoice.inputNoiseFloor()),
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getFreePsram()));
}

void startVoice() {
  char device[65] = {};
  char token[48] = {};
  char session[17] = {};
  preferences.getString(cfg::nvs::kIotDevice, device, sizeof(device));
  preferences.getString(cfg::nvs::kIotToken, token, sizeof(token));
  // Present on any build flashed after conversation memory landed; mint one
  // lazily for a device whose credential predates it.
  preferences.getString(cfg::nvs::kIotSession, session, sizeof(session));
  if (session[0] == '\0') {
    mintVoiceSession();
    preferences.getString(cfg::nvs::kIotSession, session, sizeof(session));
  }
  logPrintf("[voice] activating (credential=%s, microphone=%s)\n",
                isIotGatewayToken(token) ? "ready" : "missing",
                microphoneInput.available() ? "ready" : "missing");

  voiceStarted = buntyVoice.begin(&microphoneInput, IOT_GATEWAY_HOST, device, token, session);
  if (voiceStarted) {
    Serial.println("[voice] activation complete");
    return;
  }
  nextVoiceStartAt = millis() + cfg::voice::kStartRetryMs;
  logPrintf("[voice] activation failed; retrying in %lu ms\n",
                static_cast<unsigned long>(cfg::voice::kStartRetryMs));
}

void serviceVoice() {
  const uint32_t now = millis();
  logVoiceHeartbeat(now);
  if (!runtimeStarted || greeting::playing()) return;
  if (!ESP.getPsramSize()) return;

  serviceClockSync();

  if (!voiceStarted) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (static_cast<int32_t>(now - nextVoiceStartAt) < 0) return;
    startVoice();
    return;
  }

  // A conversation counts as interaction. Without this the idle timer would put
  // Bunty to sleep mid-sentence, and only a screen touch could wake it again.
  if (buntyVoice.engaged()) {
    lastInteractionAt = millis();
    if (displayState == DisplayState::Sleeping) wake();
  }

  serviceVoiceFace();
}

// ----------------------------------------------------------------- boot ---

void setupServos() {
  if (!cfg::servo::kEnabled) return;
  Preferences calibration;
  const bool stored = calibration.begin("bunty-motion", false);
  const auto start = [&](ServoMotion &motion, bool enabled, int pin,
                        uint8_t channel, uint8_t &home, const char *label) {
    if (!enabled) return false;
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
  power::onRevoked(power::Load::kTurnServo, []() { turnServo.hold(); });
  power::onRevoked(power::Load::kTiltServo, []() { tiltServo.hold(); });
  turnServoReady = start(turnServo, cfg::servo::kTurnEnabled,
                         cfg::pin::kTurnServo, cfg::servo::kTurnChannel,
                         turnHome, "turn");
  tiltServoReady = start(tiltServo, cfg::servo::kTiltEnabled,
                         cfg::pin::kTiltServo, cfg::servo::kTiltChannel,
                         tiltHome, "tilt");
  if (stored) calibration.end();
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
  if (!ESP.getPsramSize()) Serial.println("[voice] unavailable: full voice buffering requires PSRAM");

  setupServos();

  // The amp idles shut down; the greeting and voice wake it for their audio.
  pinMode(cfg::pin::kAmpShutdown, OUTPUT);
  digitalWrite(cfg::pin::kAmpShutdown, LOW);

  preferences.begin(cfg::nvs::kNamespace, false);
  power::begin(preferences);
  hearing::begin();
  syncIotGatewayCredential();
  loadIdentity();
  dashboard::begin(preferences, hardwareId);

  const bool touchReady = touch::begin();
  microphoneInput.begin();  // Optional for pairing; required only for voice.
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
  // Only imminent playback holds motion off; listening leaves the head free.
  const BuntyVoice::State voiceState = buntyVoice.state();
  power::setAudioExpected(greeting::playing() || buntyVoice.speaking() ||
                          voiceState == BuntyVoice::State::Thinking ||
                          voiceState == BuntyVoice::State::Draining);
  power::service();
  // Voice activation must not sit behind dashboard discovery/TCP. A sleeping or
  // unreachable Mac can block that local request for several seconds.
  serviceVoice();
  serviceLookAtSpeaker();

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
      lastGreetingBarAt = millis();
      greeting::start();
      startRuntime(false);
    }
  } else if (runtimeStarted) {
    serviceRuntime();
  }

  if (greetingEyesShown && greeting::playing()) {
    {
      ui::Guard guard;
      animations->serviceSpeaking(now);
    }
    if (now - lastGreetingBarAt >= 1000) {
      lastGreetingBarAt = now;
      ui::Guard guard;
      ui::drawTopBar(true);
    }
  }
  delay(8);
}
