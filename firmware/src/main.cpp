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
#include "ServoMotion.h"
#include "Memory.h"
#include "InternalAllocation.h"
#include "TapGestures.h"
#include "TapInput.h"
#include "flow32/graphics/Display.h"

namespace {

Preferences preferences;
TapInput tapInput;
TapGestures tapGestures(tapInput);
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
  tapGestures.reset();
  dashboard::restart();

  if (reconnectWifi) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(preferences.getString(cfg::nvs::kWifiSsid, "").c_str(),
               preferences.getString(cfg::nvs::kWifiPassword, "").c_str());
  }
  // startRuntime(false) follows a successful pairing connection, so apply the
  // reliability setting on both the cold-boot and newly-paired paths.
  WiFi.setSleep(false);
  lastWifiStatus = WiFi.status();
  nextVoiceStartAt = 0;
  if (!greeting::playing()) dashboard::render();
}

void enterSleep() {
  displayState = DisplayState::Sleeping;
  tapGestures.reset();
  tapInput.reset();
  tapInput.suppressFor(500);
  {
    ui::Guard guard;
    animations->beginSleeping(millis());
  }
  ui::fadeBacklightTo(cfg::ui::kSleepBacklightDuty);
  Serial.println("[display] sleeping; background sync remains active");
}

void wake() {
  displayState = DisplayState::Waking;
  tapGestures.reset();
  // The wake knock and its enclosure echoes must never seed an unpair gesture
  // when the dashboard returns.
  tapInput.reset();
  tapInput.suppressFor(BuntyAnimations::wakeDurationMs() + 400);
  {
    ui::Guard guard;
    animations->beginWaking(millis());
  }
  ui::fadeBacklightTo(255);
  Serial.println("[display] wake tap accepted");
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
    if (turnServoReady) turnServo.moveTo(turnAngle);
    if (tiltServoReady) tiltServo.moveTo(tiltAngle);
  };

  switch (sweepPhase) {
    case SweepPhase::Idle:
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
      sweepPhase = SweepPhase::Idle;
      return true;
  }
  return true;
}

void applyIntents(const TapGestures::Intents &intents) {
  if (intents.interacted) lastInteractionAt = millis();
  if (intents.armVoice) buntyVoice.arm();
  if (intents.wake) wake();
  if (intents.promptUnpair) ui::showUnpairPrompt();
  if (intents.cancelPrompt) {
    tapInput.reset();
    tapInput.suppressFor(500);
    dashboard::render();
  }
  if (intents.confirmUnpair) forgetPairing();
}

// Returns true while a face animation intentionally owns the screen.
bool serviceFace() {
  const uint32_t now = millis();
  if (displayState == DisplayState::Sleeping) {
    applyIntents(tapGestures.serviceSleeping());
    if (displayState != DisplayState::Sleeping) return true;
    ui::Guard guard;
    animations->serviceSleeping(now);
    return false;
  }
  if (displayState != DisplayState::Waking) return false;

  // Suppression normally prevents events here; draining is an extra boundary
  // between the wake gesture and the destructive runtime gesture recognizer.
  while (tapInput.poll()) {
  }
  bool stillWaking = false;
  {
    ui::Guard guard;
    stillWaking = animations->serviceWaking(now);
  }
  if (stillWaking) return true;

  displayState = DisplayState::Dashboard;
  lastInteractionAt = now;
  tapInput.reset();
  tapInput.suppressFor(500);
  dashboard::render();
  return false;
}

void serviceRuntime() {
  if (greeting::playing()) {
    tapInput.reset();
    return;
  }
  if (greetingEyesShown) {
    greetingEyesShown = false;
    displayState = DisplayState::Dashboard;
    lastInteractionAt = millis();
    dashboard::render();
  }

  if (displayState == DisplayState::Dashboard) {
    applyIntents(tapGestures.service(voiceStarted));
    if (tapGestures.awaitingConfirmation()) return;
  } else if (serviceFace()) {
    return;
  }

  if (displayState == DisplayState::Dashboard && !tapGestures.inProgress() &&
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
  // voice waits for it rather than blocking the display or tap detector.
  configTime(0, 0, "time.cloudflare.com", "time.google.com", "pool.ntp.org");
  clockSyncRequested = true;
  nextClockSyncAt = millis() + cfg::net::kClockSyncRetryMs;
  Serial.println("[time] requesting NTP synchronization for TLS");
}

// Bunty's face while it answers: the speaking eyes plus a vector mouth whose
// opening follows the audio actually reaching the amplifier.
void serviceVoiceFace() {
  const uint32_t now = millis();
  const bool onDashboard = displayState == DisplayState::Dashboard;
  const bool speaking = buntyVoice.speaking() && onDashboard;
  // Show Bunty's face the moment a tap arms voice, so the tap has visible
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
                tapInput.available() ? "ready" : "missing");

  voiceStarted = buntyVoice.begin(&tapInput, IOT_GATEWAY_HOST, device, token, session);
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
  // Bunty to sleep mid-sentence, and only a physical tap could wake it again.
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
  syncIotGatewayCredential();
  loadIdentity();
  dashboard::begin(preferences, hardwareId);

  const bool tapsReady = tapInput.begin();
  pairing::begin(preferences, tapInput, deviceName, hardwareId);

  if (preferences.getBool(cfg::nvs::kPaired, false)) {
    startRuntime(true);
    ui::fadeBacklight(true);
    return;
  }

  ui::showPairing();
  ui::fadeBacklight(true);
  if (!tapsReady) {
    ui::showError("Microphone not ready");
    return;
  }
  if (!pairing::start()) ui::showError(pairing::error());
}

void loop() {
  if (!setupReady) { delay(100); return; }
  // Voice activation must not sit behind dashboard discovery/TCP. A sleeping or
  // unreachable Mac can block that local request for several seconds.
  serviceVoice();

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
      greeting::start(&tapInput);
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
