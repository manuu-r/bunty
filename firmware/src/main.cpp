#include "Log.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <time.h>

#include "Animations.h"
#include "AudioIo.h"
#include "Dance.h"
#include "IotGatewayCredentials.h"
#include "Voice.h"
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

wl_status_t lastWifiStatus = WL_NO_SHIELD;

Microphone microphone;
Voice voice;
Dance dance;

// Bunty's face owns the screen for the whole conversation.
bool voiceFaceShown = false;
Voice::Phase shownPhase = Voice::Phase::Off;
uint32_t lastFaceFrameAt = 0;
uint8_t waveform[cfg::voice::kWaveformSamples] = {};
size_t waveformHead = 0;
float mouthLevel = 0.0f;

bool clockSyncRequested = false;
uint32_t nextClockSyncAt = 0;
bool publicDnsConfigured = false;

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

// ---------------------------------------------------------- iot gateway ---

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

// Scopes the gateway's short-term conversation memory. Minted whenever a new
// credential is stored, i.e. on every provisioned flash, and kept across
// power cycles.
void mintVoiceSession() {
  char sessionId[17];
  snprintf(sessionId, sizeof(sessionId), "%08x%08x",
           static_cast<unsigned>(esp_random()),
           static_cast<unsigned>(esp_random()));
  preferences.putString(cfg::nvs::kIotSession, sessionId);
  logPrintf("[iot-gateway] new conversation session %s\n", sessionId);
}

// A provisioned build carries a fresh token; copy it into NVS so ordinary
// uploads afterwards keep working with the stored one.
void syncIotGatewayCredential() {
  constexpr char compiledToken[] = IOT_GATEWAY_TOKEN;
  constexpr char compiledDevice[] = IOT_GATEWAY_DEVICE_ID;
  if (!isIotGatewayToken(compiledToken)) {
    Serial.println("[iot-gateway] no credential embedded in this build");
    return;
  }
  if (preferences.getString(cfg::nvs::kIotToken, "") == compiledToken &&
      preferences.getString(cfg::nvs::kIotDevice, "") == compiledDevice) {
    return;
  }
  const bool saved =
      preferences.putString(cfg::nvs::kIotToken, compiledToken) > 0 &&
      preferences.putString(cfg::nvs::kIotDevice, compiledDevice) > 0;
  Serial.println(saved ? "[iot-gateway] stored flashed credential"
                       : "[iot-gateway] could not store flashed credential");
  if (saved) mintVoiceSession();
}

void startVoice() {
  char token[48] = {};
  char session[17] = {};
  preferences.getString(cfg::nvs::kIotToken, token, sizeof(token));
  preferences.getString(cfg::nvs::kIotSession, session, sizeof(session));
  if (isIotGatewayToken(token) && session[0] == '\0') {
    mintVoiceSession();
    preferences.getString(cfg::nvs::kIotSession, session, sizeof(session));
  }
  voice.begin(&microphone, isIotGatewayToken(token) ? token : "", session);
}

// TLS certificate validity depends on a real clock; NTP runs asynchronously.
void serviceClockSync() {
  if (WiFi.status() != WL_CONNECTED) {
    publicDnsConfigured = false;
    return;
  }
  if (!publicDnsConfigured) {
    // Some routers' DNS relays fail intermittently for the gateway host.
    esp_netif_t *station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
    if (station && esp_netif_set_dns_info(station, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
      esp_netif_dns_info_t backup = {};
      backup.ip.type = ESP_IPADDR_TYPE_V4;
      backup.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
      esp_netif_set_dns_info(station, ESP_NETIF_DNS_BACKUP, &backup);
    }
    publicDnsConfigured = true;
  }
  if (time(nullptr) >= cfg::net::kMinimumTrustedUnixTime) return;
  if (clockSyncRequested && static_cast<int32_t>(millis() - nextClockSyncAt) < 0) return;
  configTime(0, 0, "time.cloudflare.com", "time.google.com", "pool.ntp.org");
  clockSyncRequested = true;
  nextClockSyncAt = millis() + cfg::net::kClockSyncRetryMs;
  Serial.println("[time] requesting NTP synchronization for TLS");
}

// -------------------------------------------------------------- runtime ---

void startRuntime(bool reconnectWifi) {
  runtimeStarted = true;
  displayState = DisplayState::Dashboard;
  lastInteractionAt = millis();
  dashboard::restart();
  // Commissioning has ended (or this boot was already paired). Reclaim BLE
  // before allocating voice tasks and I2S DMA; no audio is needed in setup.
  pairing::releaseMemory();
  if (!microphone.available() && microphone.begin()) startVoice();

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

// ---------------------------------------------------------------- voice ---

const char *phaseLabel(Voice::Phase phase) {
  switch (phase) {
    case Voice::Phase::Connecting: return "CONNECTING";
    case Voice::Phase::Listening: return "LISTENING";
    case Voice::Phase::Thinking:
    case Voice::Phase::Finishing: return "THINKING";
    case Voice::Phase::Speaking: return "";
    case Voice::Phase::Off: break;
  }
  return "";
}

void showVoiceFace(uint32_t now) {
  voiceFaceShown = true;
  shownPhase = Voice::Phase::Off;
  waveformHead = 0;
  mouthLevel = 0.0f;
  memset(waveform, 0, sizeof(waveform));
  ui::Guard guard;
  ui::display().clear(cfg::ui::kBackground);
  animations->beginSpeaking(now, false);
  ui::drawTopBar(false);
  ui::display().present();
}

void hideVoiceFace() {
  voiceFaceShown = false;
  memory::report();  // the peak of a whole conversation, while it's fresh
  touch::flush();
  lastInteractionAt = millis();
  const char *error = voice.lastError();
  if (*error) {
    ui::showError(error);
    delay(1500);
  }
  if (displayState == DisplayState::Dashboard) dashboard::render();
}

// Eyes, a phase label, the microphone level while listening, and a mouth
// that opens with the reply actually reaching the speaker.
void serviceVoiceFace(uint32_t now) {
  const Voice::Phase phase = voice.phase();
  animations->setEmotion(phase == Voice::Phase::Speaking ? voice.emotion()
                                                         : Emotion::Neutral);
  if (phase != shownPhase) {
    const bool wasSpeaking = shownPhase == Voice::Phase::Speaking;
    shownPhase = phase;
    ui::Guard guard;
    const char *label = phaseLabel(phase);
    if (*label) animations->drawWakeIcon(label);
    else animations->clearWakeIcon();
    if (wasSpeaking) animations->drawSpeakingMouth(0);
    if (phase != Voice::Phase::Listening) animations->clearInputWaveform();
  }
  {
    ui::Guard guard;
    animations->serviceSpeaking(now);
  }
  if (now - lastFaceFrameAt < cfg::voice::kFaceFrameMs) return;
  lastFaceFrameAt = now;

  if (phase == Voice::Phase::Listening) {
    uint32_t scaled = voice.inputLevel() * 100u / cfg::voice::kWaveformFullScaleRms;
    waveform[waveformHead] = static_cast<uint8_t>(scaled > 100 ? 100 : scaled);
    waveformHead = (waveformHead + 1) % cfg::voice::kWaveformSamples;
    ui::Guard guard;
    animations->drawInputWaveform(waveform, cfg::voice::kWaveformSamples, waveformHead);
  } else if (phase == Voice::Phase::Speaking) {
    const float level = voice.outputLevel();
    mouthLevel += (level - mouthLevel) * (level > mouthLevel ? 0.6f : 0.3f);
    float openness = mouthLevel * 120.0f;
    ui::Guard guard;
    animations->drawSpeakingMouth(static_cast<uint8_t>(openness > 100.0f ? 100.0f : openness));
  }
}

// Returns true while the conversation owns the screen and touch.
bool serviceVoice() {
  const uint32_t now = millis();
  dance.service(voice.phase(), voice.emotion(), now);
  if (!voice.active()) {
    if (voiceFaceShown) hideVoiceFace();
    return false;
  }
  lastInteractionAt = now;
  if (displayState != DisplayState::Dashboard || overlay != Overlay::kNone) {
    overlay = Overlay::kNone;
    displayState = DisplayState::Dashboard;
    ui::fadeBacklightTo(255);
  }
  if (!voiceFaceShown) showVoiceFace(now);
  // Anywhere on the face is the mic button: end, or interrupt the reply.
  if (touch::poll().kind == touch::Event::Kind::kTap) voice.toggle();
  serviceVoiceFace(now);
  return true;
}

void serviceRuntime() {
  if (greetingEyesShown && static_cast<int32_t>(millis() - greetingEndsAt) < 0) return;
  if (greetingEyesShown) {
    greetingEyesShown = false;
    displayState = DisplayState::Dashboard;
    lastInteractionAt = millis();
    dashboard::render();
  }

  serviceClockSync();
  const bool conversing = serviceVoice();

  if (conversing) {
    // The dashboard keeps syncing behind the face; it just doesn't draw.
  } else if (displayState == DisplayState::Dashboard) {
    const touch::Event event = touch::poll();
    if (event.kind != touch::Event::Kind::kNone) {
      lastInteractionAt = millis();
      power::markBusy();
    }
    if (overlay != Overlay::kNone) {
      serviceSettingsTouch(event);
      return;
    }
    if (event.kind == touch::Event::Kind::kTap && ui::voiceControlAt(event.x, event.y)) {
      if (voice.ready()) {
        voice.toggle();
      } else {
        ui::showError("Voice not set up");
        delay(1200);
        dashboard::render();
      }
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

  if (!conversing && displayState == DisplayState::Dashboard && overlay == Overlay::kNone &&
      millis() - lastInteractionAt >= cfg::dashboard::kIdleSleepMs) {
    enterSleep();
  }

  const wl_status_t wifiStatus = WiFi.status();
  if (wifiStatus != lastWifiStatus) {
    lastWifiStatus = wifiStatus;
    dashboard::forgetDiscovery();
    dashboard::refreshSoon();
    if (displayState == DisplayState::Dashboard && !conversing) {
      dashboard::render();
    } else {
      ui::Guard guard;
      ui::drawTopBar(true);
    }
  }

  if (dashboard::serviceFetch() && displayState == DisplayState::Dashboard && !conversing) {
    dashboard::render();
  }

  if (displayState == DisplayState::Dashboard && !conversing && dashboard::pageDue()) {
    dashboard::nextPage();
    dashboard::render();
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

// Bench check for cfg::ui::kFaceDemo: every emotion's face, five seconds each.
void runFaceDemo() {
  static const char *const kNames[] = {
      "NEUTRAL", "SOFT", "HAPPY", "EXCITED", "ANGRY", "SAD", "CURIOUS", "SASSY",
  };
  ui::fadeBacklight(true);
  for (uint8_t index = 0;; index = (index + 1) % uint8_t(Emotion::Count)) {
    logPrintf("[demo] %s\n", kNames[index]);
    const uint32_t start = millis();
    {
      ui::Guard guard;
      ui::display().clear(cfg::ui::kBackground);
      animations->setEmotion(static_cast<Emotion>(index));
      animations->beginSpeaking(start, false);
      ui::drawTopBar(false);
      ui::display().present();
      animations->drawWakeIcon(kNames[index]);
    }
    while (millis() - start < 5000) {
      const uint32_t now = millis();
      // A stand-in speech envelope, so the mouth moves as it would in a reply.
      const float wave = fabsf(sinf(now * 0.012f)) * (0.5f + 0.5f * sinf(now * 0.003f));
      ui::Guard guard;
      animations->serviceSpeaking(now);
      animations->drawSpeakingMouth(static_cast<uint8_t>(wave * 100.0f));
      delay(20);
    }
  }
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
  dance.attach(turnServoReady ? &turnServo : nullptr, turnHome,
               tiltServoReady ? &tiltServo : nullptr, tiltHome);
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

  if (cfg::ui::kFaceDemo) runFaceDemo();
  setupServos();

  // The amp idles shut down; the speaker wakes it only for a reply.
  pinMode(cfg::pin::kAmpShutdown, OUTPUT);
  digitalWrite(cfg::pin::kAmpShutdown, LOW);

  preferences.begin(cfg::nvs::kNamespace, false);
  power::begin(preferences);
  loadIdentity();
  syncIotGatewayCredential();
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

  // Heap and task-stack headroom: once shortly after boot, then every 5 min.
  memory::sampleTask("loop");
  static uint32_t nextMemoryReportAt = 30000;
  if (static_cast<int32_t>(now - nextMemoryReportAt) >= 0) {
    nextMemoryReportAt = now + 5 * 60 * 1000;
    memory::report();
  }
  delay(8);
}

// Native ESP-IDF entry point; retain the existing application state machine.
extern "C" void app_main() {
  platformInit();
  setup();
  for (;;) loop();
}
