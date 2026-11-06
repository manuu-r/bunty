#include "Log.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <time.h>

#include "Animations.h"
#include "AudioIo.h"
#include "Dance.h"
#include "IotGatewayCredentials.h"
#include "Voice.h"
#include "WakeWord.h"
#include "WakeInfo.h"
#include "Muse.h"
#include "MuseDisplay.h"
#include "Shell.h"
#include "VoiceAura.h"
#include "VoiceTransition.h"
#include "Fonts.h"
#include "Config.h"
#include "Ui.h"
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

bool runtimeStarted = false;
bool setupReady = false;

// Muse owns connectivity; Bunty owns the idle face, sleep, and conversation UI.
enum class DisplayState : uint8_t { Idle, Sleeping, Waking };
DisplayState displayState = DisplayState::Idle;
uint32_t lastInteractionAt = 0;

// Bunty's eyes greet the user for a moment after pairing completes.
constexpr uint32_t kGreetingMs = 2500;
bool greetingEyesShown = false;
uint32_t greetingEndsAt = 0;

wl_status_t lastWifiStatus = WL_NO_SHIELD;

Microphone microphone;
Voice voice;
Dance dance;
WakeWord wakeWord;
bool wakeStarted = false;

// Bunty's face owns the screen for the whole conversation.
bool voiceFaceShown = false;
bool remoteDisplayShown = false;
Voice::Phase shownPhase = Voice::Phase::Off;
uint32_t lastFaceFrameAt = 0;
uint32_t noticeUntil = 0;
uint32_t pairRestartAt = 0;
void showIdleFace(Emotion mood = Emotion::Neutral);
// Never words on the face: a frown and a shake of the head. The reason is logged.
void showNotice(const char *text,uint32_t duration) {
  logPrintf("[ui] %s\n",text);
  showIdleFace(Emotion::Sad);
  speechGesture::requestVisual(speechGesture::Kind::Shake,millis(),speechGesture::Speed::Normal);
  noticeUntil=millis()+duration;
}

bool clockSyncRequested = false;
uint32_t nextClockSyncAt = 0;
bool publicDnsConfigured = false;

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
}

void startVoice() {
  char token[48] = {};
  preferences.getString(cfg::nvs::kIotToken, token, sizeof(token));
  if (!voice.begin(&microphone, isIotGatewayToken(token) ? token : ""))
    Serial.println("[muse] voice initialization failed");
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

void showIdleFace(Emotion mood) {
  ui::Guard guard;
  if(muse::displayActive()) return;
  if(shell::resume()) {shell::invalidate();return;}
  animations->setEmotion(mood);
  animations->setAttention(Face::Attention::Idle);
  ui::display().clear(cfg::ui::kBackground);
  animations->beginSpeaking(millis(), false);
  if (cfg::ui::kTextContent) ui::drawControls();
  ui::display().present();
}

void startRuntime() {
  runtimeStarted = true;
  displayState = DisplayState::Idle;
  lastInteractionAt = millis();
  if (!microphone.available() && microphone.begin()) startVoice();
  if (!wakeStarted && microphone.available()) wakeStarted = wakeWord.begin(&microphone);
  power::markBusy();
  lastWifiStatus = WiFi.status();
  if (!greetingEyesShown) showIdleFace();
}

void enterSleep() {
  touch::flush();
  displayState = DisplayState::Sleeping;
  {
    ui::Guard guard;
    animations->beginSleeping(millis());
  }
  ui::fadeBacklightTo(cfg::ui::kSleepBacklightDuty);
  Serial.println("[display] sleeping; Muse connection remains active");
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

void forgetPairing() { muse::resetPairing(); }

enum class Overlay : uint8_t { kNone, kSettings, kConfirmUnpair, kConfirmDisconnect };
Overlay overlay = Overlay::kNone;

ui::SettingsStatus settingsStatus() {
  ui::SettingsStatus status;
  status.paired = muse::paired();
  status.online = muse::connected();
  status.network = muse::network();
  status.signal = status.online ? WiFi.RSSI() : 0;
  return status;
}

void openSettings() {
  shell::hide();
  overlay = Overlay::kSettings;
  touch::flush();
  ui::showSettings(settingsStatus());
}

void closeOverlay() {
  overlay = Overlay::kNone;
  touch::flush();
  showIdleFace();
}

void disconnectFromMuse() {
  muse::disconnect();
  closeOverlay();
}

void serviceSettingsTouch(const touch::Event &event) {
  if (event.kind != touch::Event::Kind::kTap) return;
  switch (overlay) {
    case Overlay::kSettings:
      switch (ui::settingsRowAt(event.x, event.y)) {
        case ui::SettingsRow::kPair:
          ui::showError("Restarting to pair");
          pairRestartAt=millis()+900;
          break;
        case ui::SettingsRow::kDisconnect:
          overlay = Overlay::kConfirmDisconnect;
          touch::flush();
          ui::showConfirm("DISCONNECT?", "Keeps saved credentials");
          break;
        case ui::SettingsRow::kUnpair:
          overlay = Overlay::kConfirmUnpair;
          touch::flush();
          ui::showConfirm("UNPAIR?", "Bunty will forget Muse setup");
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
      if (ui::confirmAcceptedAt(event.x, event.y)) disconnectFromMuse();
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
      // The waking touch must not also act on the idle face when it returns.
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

  displayState = DisplayState::Idle;
  lastInteractionAt = now;
  showIdleFace();
  return false;
}

// ---------------------------------------------------------------- voice ---

void showVoiceFace(uint32_t now) {
  voiceFaceShown = true;
  shownPhase = Voice::Phase::Off;
  ui::Guard guard;
  ui::display().clear(cfg::ui::kBackground);
  animations->beginSpeaking(now, false);
  ui::display().present();
}

void hideVoiceFace() {
  voiceFaceShown = false;
  memory::report();  // the peak of a whole conversation, while it's fresh
  touch::flush();
  lastInteractionAt = millis();
  const char *error = voice.lastError();
  if (*error) {
    showNotice(error,1500);
    return;
  }
  if (displayState == DisplayState::Idle) showIdleFace();
}

// Voice is expressed by the face and independently composed border aura.
// Pantomime companion: cute emotion eyes, physical servo gestures, punchy short reactions,
// and progress indicators for long running tasks without cluttering the screen.
void serviceVoiceFace(uint32_t now) {
  const auto phase=voice.phase();
  const bool changed=phase!=shownPhase;
  const auto prevPhase=shownPhase;
  shownPhase=phase;
  animations->setAttention(phase==Voice::Phase::Listening?Face::Attention::Listening:
    phase==Voice::Phase::Speaking?Face::Attention::Speaking:
    voice.working()?Face::Attention::Working:Face::Attention::Thinking);
  animations->setEmotion(phase==Voice::Phase::Speaking?voice.emotion():Emotion::Neutral);
  {ui::Guard guard;animations->serviceSpeaking(now);}

  // Non-verbal reaction text below the eyes during speaking/reacting phase:
  static bool reactionShown=false;
  if(phase==Voice::Phase::Speaking) {
    if(!reactionShown || changed) {
      char cap[64]={};
      muse::caption(cap,sizeof(cap));
      if(cap[0]) {
        ui::Guard guard;
        animations->drawReaction(cap);
        reactionShown=true;
      }
    }
  } else if(reactionShown || (changed && prevPhase==Voice::Phase::Speaking)) {
    ui::Guard guard;
    animations->clearReaction();
    reactionShown=false;
  }

  // Live progress indicator during long-running tasks:
  static bool progressShown=false;
  static uint32_t lastProgressFrame=0;
  if(voice.working()) {
    char status[32]={};
    int percent=-1;
    const bool hasProgress=muse::getProgress(status,sizeof(status),percent);
    if(now-lastProgressFrame>=60 || !progressShown) {
      lastProgressFrame=now;
      ui::Guard guard;
      animations->drawProgress(hasProgress && status[0] ? status : "WORKING...", percent, now);
      progressShown=true;
    }
  } else if(progressShown) {
    ui::Guard guard;
    animations->clearProgress();
    progressShown=false;
  }

  if(!changed && now-lastFaceFrameAt<cfg::voice::kFaceFrameMs)return;
  lastFaceFrameAt=now;
  const float level=phase==Voice::Phase::Speaking?voice.outputLevel():0;
  ui::Guard guard;
  if(!reactionShown) {
    animations->drawSpeakingMouth(uint8_t(level*100.f));
  }
}

// Returns true while the conversation owns the screen and touch.
bool serviceVoice() {
  const uint32_t now = millis();
  dance.service(voice, now);
  if (!voice.active()) {
    if (voiceFaceShown) hideVoiceFace();
    return false;
  }
  lastInteractionAt = now;
  if (displayState != DisplayState::Idle || overlay != Overlay::kNone) {
    overlay = Overlay::kNone;
    displayState = DisplayState::Idle;
    ui::fadeBacklightTo(255);
  }
  if (!voiceFaceShown) showVoiceFace(now);
  const auto event=touch::poll();
  if(event.kind==touch::Event::Kind::kLongPress)voice.end();
  else if(event.kind==touch::Event::Kind::kTap &&
          (voice.phase()==Voice::Phase::Speaking || voice.phase()==Voice::Phase::Thinking))
    voice.end(); // a touch stops the conversation; only "Hey Buddy" starts one
  serviceVoiceFace(now);
  return true;
}

// "Hey Buddy": turn toward the voice, then open the conversation. The only way in.
void onWake(const WakeEvent &heard, uint32_t now) {
  lastInteractionAt = now;
  power::markBusy();
  if (displayState == DisplayState::Sleeping) wake();
  // The mics ride on the head, so the bearing is relative to where it points now.
  const float current = turnServoReady ? turnServo.angle() - float(turnHome) : 0.0f;
  float yaw = 0.0f;
  bool turning = false;
  if (heard.bearingValid && fabsf(heard.bearingDegrees) >= cfg::wake::kTurnDeadbandDegrees) {
    yaw = fmaxf(-cfg::wake::kMaxTurnDegrees,
                fminf(cfg::wake::kMaxTurnDegrees, current + heard.bearingDegrees * cfg::wake::kRightIsPositiveYaw));
    turning = true;
  }
  logPrintf("[wake] head at %+.0f deg; %s\n", current,
            turning ? "turning toward the voice" : "bearing unusable or ahead; not turning");
  wakeInfo::Record record;
  record.atMs = now;
  record.bearingValid = heard.bearingValid;
  record.bearingRight = heard.bearingDegrees;
  record.confidence = heard.confidence;
  record.turned = turning;
  record.lookYaw = turning ? yaw : current;
  if (cfg::wake::kStartConversation && voice.ready() && !voice.active()) {
    record.conversationStarted = true;
    shell::hide();
    // The conversation owns the head, so the turn is held by Dance, not queued.
    dance.setBearing(turning ? yaw : 0.0f, now);
    voice.startFromWake();
  } else if (turning) {
    // No conversation will open: turn on its own, as bunty.look would.
    muse::lookAt(yaw, 0.0f, speechGesture::rate(speechGesture::Speed::Fast));
  }
  wakeInfo::record(record);
}

void serviceWake() {
  const uint32_t now = millis();
  // The head's own servos are loud next to its microphones.
  if (turnServo.moving() || tiltServo.moving()) voice.noteMotion(now);
  // Listen only while nothing else uses the microphone.
  wakeWord.setArmed(wakeWord.available() && !voice.active() && !muse::setupActive());
  WakeEvent heard;
  if (wakeWord.take(heard)) onWake(heard, now);
}

void serviceRuntime() {
  serviceClockSync();
  // One UI transaction per loop prevents asynchronous image/text commands
  // from taking ownership between a main-task check and its drawing calls.
  ui::Guard guard;
  if(muse::displayActive()) {
    noticeUntil=0;
    remoteDisplayShown=true; voiceFaceShown=false;
    overlay=Overlay::kNone; displayState=DisplayState::Idle;
    ui::fadeBacklightTo(255);
    dance.service(voice,millis());
    if(muse::imageFetching() || voice.active()) power::markBusy();
    lastInteractionAt=millis();
    const auto event=touch::poll();
    if(event.kind==touch::Event::Kind::kTap) {
      const bool spoken=muse::speechPictureActive();
      muse::tapDisplay();
      if(spoken)voice.end(); // the picture is part of the reply being interrupted
    } else if(event.kind==touch::Event::Kind::kLongPress) {
      if(voice.active())voice.end();
      else {muse::dismissDisplay();openSettings();}
    }
    return;
  }
  if(pairRestartAt) return;
  if(noticeUntil) {
    dance.service(voice,millis());
    if(static_cast<int32_t>(millis()-noticeUntil)<0) return;
    noticeUntil=0; showIdleFace();
  }
  if(remoteDisplayShown) { remoteDisplayShown=false; if(!voice.active()) showIdleFace(); }
  if(shell::takeFaceRequest()) showIdleFace();
  if (greetingEyesShown && static_cast<int32_t>(millis() - greetingEndsAt) < 0) return;
  if (greetingEyesShown) {
    greetingEyesShown = false;
    displayState = DisplayState::Idle;
    lastInteractionAt = millis();
    showIdleFace();
  }

  if(shell::visible()) {
    voiceFaceShown=false; displayState=DisplayState::Idle; overlay=Overlay::kNone;
    dance.service(voice,millis());
    const uint32_t now=millis();
    auto event=touch::poll();
    const bool dimmed=!voice.active() && now-shell::activityAt()>=cfg::ui::kIdleSleepMs;
    if(event.kind!=touch::Event::Kind::kNone) {
      power::markBusy();shell::wake(now);
      if(dimmed) event={}; // the waking touch must not check a task
    }
    ui::fadeBacklightTo(dimmed && event.kind==touch::Event::Kind::kNone &&
                        now-shell::activityAt()>=cfg::ui::kIdleSleepMs?cfg::ui::kSleepBacklightDuty:255);
    const auto action=shell::service(event,muse::connected(),voice.phase()==Voice::Phase::Speaking,now);
    if(action==shell::Action::Face) {
      shell::showFace();shell::takeFaceRequest();showIdleFace();
    } else if(action==shell::Action::Settings) {
      if(voice.active())voice.end();else openSettings();
    }
    if(voice.active()) power::markBusy();
    return;
  }
  const bool conversing = serviceVoice();
  if(noticeUntil || shell::visible()) return;

  if (!conversing && displayState == DisplayState::Idle) {
    const touch::Event event = touch::poll();
    if (event.kind != touch::Event::Kind::kNone) {
      lastInteractionAt = millis();
      power::markBusy();
    }
    if (overlay != Overlay::kNone) {
      serviceSettingsTouch(event);
      return;
    }
    if(cfg::ui::kTextContent && event.kind==touch::Event::Kind::kTap && ui::settingsControlAt(event.x,event.y)) {
      shell::showHome();return;
    }
    if(event.kind==touch::Event::Kind::kLongPress) {openSettings();return;}
  } else if (serviceFace()) {
    return;
  }

  if (!conversing && displayState == DisplayState::Idle && overlay == Overlay::kNone &&
      millis() - lastInteractionAt >= cfg::ui::kIdleSleepMs) {
    enterSleep();
  }

  const wl_status_t wifiStatus = WiFi.status();
  if (wifiStatus != lastWifiStatus) {
    lastWifiStatus = wifiStatus;
    if (displayState == DisplayState::Idle && !conversing) {
      showIdleFace();
    }
  }

  if (!conversing && displayState == DisplayState::Idle && overlay == Overlay::kNone) {
    ui::Guard guard;
    animations->serviceSpeaking(millis());
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
          ui::display().present();
      animations->drawWakeIcon(kNames[index]);
    }
    while (millis() - start < 5000) {
      ui::serviceBacklight();
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
  if(!shell::begin()) Serial.println("[ui] shell storage unavailable; using face controls");
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

  if (!preferences.begin(cfg::nvs::kNamespace, false)) {
    setupReady = false;
    ui::showError("Local storage unavailable");
    ui::fadeBacklight(true);
    return;
  }
  power::begin(preferences);
  syncIotGatewayCredential();

  const bool touchReady = touch::begin();
  const bool museStarted = muse::begin();

  if (!museStarted) {
    setupReady = false;
    ui::showError("Muse initialization failed");
    ui::fadeBacklight(true);
    return;
  }
  if (muse::paired()) {
    startRuntime();
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
}

// Decoding and drawing take the display, so they happen here and not on the
// audio task that fires the cue. A cue precedes the phase change it belongs to,
// so a picture this new is not yet evidence that speech has ended.
void serviceSpeechPicture() {
  static uint32_t shownAt=0;
  const uint8_t *data=nullptr;size_t bytes=0;
  if(voice.takePicture(data,bytes)) {
    if(muse::showSpeechPicture(data,bytes))shownAt=millis();
    voice.releasePicture();
  } else if(muse::speechPictureActive() && voice.phase()!=Voice::Phase::Speaking && millis()-shownAt>=500) {
    muse::endSpeechPicture();
  }
}

void serviceVoiceAura() {
  if(!runtimeStarted)return;
  static uint32_t last=0;
  static Voice::Phase previous=Voice::Phase::Off;
  const auto phase=voice.phase();const uint32_t now=millis();
  if(phase==previous && now-last<100)return;
  static voiceAura::Frame source{},current{};
  static uint32_t changedAt=0;
  if(phase!=previous){source=current;changedAt=now;}
  previous=phase;last=now;
  voiceAura::State state=voiceAura::State::Off;
  switch(phase) {
    case Voice::Phase::Listening:state=voiceAura::State::Listening;break;
    case Voice::Phase::Thinking:state=voice.working()?voiceAura::State::Working:voiceAura::State::Thinking;break;
    case Voice::Phase::Speaking:state=voiceAura::State::Speaking;break;
    case Voice::Phase::Connecting:state=voiceAura::State::Connecting;break;
    case Voice::Phase::Finishing:state=voiceAura::State::Finishing;break;
    case Voice::Phase::Off:break;
  }
  const unsigned level=phase==Voice::Phase::Speaking?unsigned(voice.outputLevel()*100):
    unsigned(uint64_t(voice.inputLevel())*100/cfg::voice::kInputFullScaleRms);
  const auto target=voiceAura::frame(state,now,level);
  const auto aura=voiceAura::blend(source,target,voiceScreen::transition(now,changedAt));
  current=aura;
  ui::Guard guard;ui::display().setAura(aura.color,aura.opacity);
}

void loop() {
  const int64_t loopStarted=esp_timer_get_time();
  ui::serviceBacklight();
  if(pairRestartAt && static_cast<int32_t>(millis()-pairRestartAt)>=0) {
    pairRestartAt=0; forgetPairing();
  }
  if (!setupReady) { delay(100); return; }
  power::service();
  serviceVoiceAura();
  serviceSpeechPicture();
  muse::serviceDisplay();
  muse::service(turnServoReady ? &turnServo : nullptr, tiltServoReady ? &tiltServo : nullptr, turnHome, tiltHome, voice.phase()!=Voice::Phase::Off || dance.active() || speechGesture::visualWaiting());

  // Servo trajectories are serviced independently by their core-1 workers.
  const uint32_t now = millis();

  if (muse::setupActive()) {
    if (muse::serviceSetup()) {
      // Commissioned: greet the user, then bring the idle face up behind it.
      {
        ui::Guard guard;
        ui::display().fillScreen(cfg::ui::kBackground);
              animations->beginSpeaking(millis(), false);
        ui::display().present();
      }
      greetingEyesShown = true;
      greetingEndsAt = millis() + kGreetingMs;
      startRuntime();
    }
  } else if (runtimeStarted) {
    serviceWake();
    serviceRuntime();
  }

  if (greetingEyesShown && static_cast<int32_t>(now - greetingEndsAt) < 0) {
    ui::Guard guard;
    if(!muse::displayActive()) animations->serviceSpeaking(now);
  }

  // Heap and task-stack headroom: once shortly after boot, then every 5 min.
  memory::sampleTask("loop");
  static uint32_t nextMemoryReportAt = 30000;
  if (static_cast<int32_t>(now - nextMemoryReportAt) >= 0) {
    nextMemoryReportAt = now + 5 * 60 * 1000;
    memory::report();
  }
  memory::recordLoop(uint32_t(esp_timer_get_time()-loopStarted));
  const bool activeLoop = (displayState != DisplayState::Sleeping) || voice.active();
  delay(activeLoop ? 1 : 8);
}

// Report the previous reset before display/radio startup, including brownouts,
// panics and watchdogs. USB resets caused by opening a monitor are distinct.
const char *resetReasonName(esp_reset_reason_t reason) {
  switch(reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    default: return "other";
  }
}
extern "C" void app_main() {
  vTaskPrioritySet(nullptr, 3);
  const auto reason=esp_reset_reason();
  logPrintf("[boot] previous reset=%s (%d)\n",resetReasonName(reason),int(reason));
  if (!platformInit()) {
    // Preserve NVS and stop the reboot loop on an initialization failure.
    for (;;) delay(1000);
  }
  setup();
  for (;;) loop();
}
