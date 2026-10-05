#include "Voice.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_tls.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "Log.h"
#include "Power.h"
#include "VoiceCerts.h"

namespace {

using namespace cfg::voice;

constexpr uint8_t kOpContinuation = 0x0;
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;

constexpr size_t kArenaBytes = 16384;
constexpr size_t kPlaySamples = 512;

bool trustStoreReady = false;

bool ensureTrustStore() {
  if (trustStoreReady) return true;
  if (esp_tls_init_global_ca_store() != ESP_OK ||
      esp_tls_set_global_ca_store(
          reinterpret_cast<const unsigned char *>(kIotGatewayRootCertificates),
          strlen(kIotGatewayRootCertificates) + 1) != ESP_OK) {
    Serial.println("[voice] could not load the gateway root certificates");
    return false;
  }
  trustStoreReady = true;
  return true;
}

bool clockTrusted() { return time(nullptr) >= cfg::net::kMinimumTrustedUnixTime; }

void copyCode(char *destination, size_t capacity, const char *source) {
  strncpy(destination, source ? source : "", capacity - 1);
  destination[capacity - 1] = '\0';
}

}  // namespace

const char *Voice::phaseName(Phase phase) {
  switch (phase) {
    case Phase::Off: return "off";
    case Phase::Connecting: return "connecting";
    case Phase::Listening: return "listening";
    case Phase::Thinking: return "thinking";
    case Phase::Speaking: return "speaking";
    case Phase::Finishing: return "finishing";
  }
  return "?";
}

bool Voice::begin(Microphone *microphone, const char *token, const char *sessionId) {
  if (ready_) return true;
  if (!microphone || !microphone->available()) {
    Serial.println("[voice] microphone unavailable; voice disabled");
    return false;
  }
  if (!token || strncmp(token, "iot_", 4) != 0) {
    Serial.println("[voice] no gateway credential; flash with BUNTY_PROVISION_IOT=1");
    return false;
  }
  if (!ensureTrustStore()) return false;
  microphone_ = microphone;

  snprintf(uri_, sizeof(uri_), "wss://%s%s", kGatewayHost, kLivePath);
  const bool hasSession = sessionId && *sessionId;
  const int length = snprintf(
      headers_, sizeof(headers_),
      "Authorization: Bearer %s\r\nX-Bunty-Turn-Detection: gateway\r\n%s%s%s",
      token, hasSession ? "X-Bunty-Session: " : "", hasSession ? sessionId : "",
      hasSession ? "\r\n" : "");
  if (length <= 0 || length >= int(sizeof(headers_))) return false;

  auto *playbackStorage =
      static_cast<uint8_t *>(memory::external(cfg::audio::kPlaybackRingBytes + 1));
  text_ = static_cast<char *>(memory::external(kControlBytes + 1));
  arenaStorage_ = static_cast<uint8_t *>(memory::external(kArenaBytes));
  frame_ = static_cast<uint8_t *>(memory::external(kUploadFrameBytes));
  events_ = xQueueCreate(24, sizeof(Event));
  cues_ = xQueueCreate(16, sizeof(Cue));
  if (!playbackStorage || !text_ || !arenaStorage_ || !frame_ || !events_ || !cues_) {
    Serial.println("[voice] could not allocate voice buffers");
    return false;
  }
  playback_ = xStreamBufferCreateStatic(cfg::audio::kPlaybackRingBytes + 1, 1,
                                        playbackStorage, &playbackControl_);
  arena_.bind(arenaStorage_, kArenaBytes);

  if (xTaskCreatePinnedToCore(voiceTask, "voice", 6144, this, 4, nullptr, 0) != pdPASS ||
      xTaskCreatePinnedToCore(speakTask, "speak", 4096, this, 5, nullptr, 1) != pdPASS) {
    Serial.println("[voice] could not start voice tasks");
    return false;
  }
  ready_ = true;
  logPrintf("[voice] ready for %s; tap the mic to talk\n", uri_);
  return true;
}

// ------------------------------------------------------------ voice task ---

void Voice::voiceTask(void *context) { static_cast<Voice *>(context)->run(); }

void Voice::run() {
  uint32_t sampledAt = 0;
  for (;;) {
    const uint32_t now = millis();
    if (toggleRequested_.exchange(false)) {
      switch (phase_.load()) {
        case Phase::Off: start(now); break;
        case Phase::Connecting:
        case Phase::Listening: endConversation("ended by tap", false); break;
        default: interrupt(now); break;
      }
    }
    Event event;
    while (xQueueReceive(events_, &event, 0) == pdTRUE) {
      if (event.generation == generation_) handle(event, millis());
    }
    step(millis());
    if (phase_ != Phase::Off) power::markBusy();
    if (now - sampledAt >= 10000) {
      sampledAt = now;
      memory::sampleTask("voice");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void Voice::setPhase(Phase phase) {
  if (phase_ == phase) return;
  logPrintf("[voice] %s -> %s\n", phaseName(phase_), phaseName(phase));
  phase_ = phase;
  phaseSince_ = millis();
}

void Voice::start(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    lastError_ = "No Wi-Fi";
    Serial.println("[voice] cannot start: Wi-Fi is offline");
    return;
  }
  lastError_ = "";
  failures_ = 0;
  lastActivityAt_ = now;
  lastLocalSpeechAt_ = 0;
  power::setRadioAwake(true);
  Serial.println("[voice] conversation started");
  nextTurn(now);
}

void Voice::nextTurn(uint32_t now) {
  closeSocket(false);
  // Everything captured while Bunty spoke is its own voice; start clean.
  microphone_->setCapturing(false);
  microphone_->discard();
  microphone_->setCapturing(true);
  retryAt_ = now;
  speechPending_ = false;
  setPhase(Phase::Connecting);
  phaseSince_ = now;
}

void Voice::endConversation(const char *reason, bool failed) {
  closeSocket(true);
  microphone_->setCapturing(false);
  microphone_->discard();
  power::setRadioAwake(false);
  if (failed) lastError_ = reason;
  logPrintf("[voice] conversation over: %s\n", reason);
  setPhase(Phase::Off);
}

void Voice::interrupt(uint32_t now) {
  Serial.println("[voice] interrupted; listening again");
  closeSocket(true);
  lastActivityAt_ = now;
  nextTurn(now);
}

void Voice::fail(const char *reason, uint32_t now) {
  logPrintf("[voice] turn failed: %s\n", reason);
  closeSocket(false);
  if (++failures_ >= kMaxConsecutiveFailures) {
    endConversation(reason, true);
    return;
  }
  nextTurn(now);
  retryAt_ = now + kRetryDelayMs * failures_;
}

void Voice::step(uint32_t now) {
  const Phase phase = phase_;
  if (phase == Phase::Off) return;

  if (WiFi.status() != WL_CONNECTED) {
    endConversation("Wi-Fi lost", true);
    return;
  }
  trackLocalSpeech(now);

  switch (phase) {
    case Phase::Connecting:
      if (!client_) {
        if (static_cast<int32_t>(now - retryAt_) < 0) return;
        // Wait for a voice before spending a transcription session on silence.
        // The microphone keeps filling its ring, so nothing said is lost.
        if (!speechPending_) return;
        if (!clockTrusted()) {
          if (now - phaseSince_ > kConnectTimeoutMs) fail("clock not synchronized", now);
          return;
        }
        connectingSince_ = now;
        if (!openSocket()) fail("could not open socket", now);
        return;
      }
      if (now - connectingSince_ > kConnectTimeoutMs) fail("no session.ready", now);
      return;

    case Phase::Listening:
      if (silenceExpired(now)) {
        endConversation("8 s of silence", false);
        return;
      }
      upload();
      return;

    case Phase::Thinking:
      if (now - phaseSince_ > kReplyTimeoutMs) fail("no reply", now);
      return;

    case Phase::Speaking:
      if (drained_.exchange(false)) {
        char body[64];
        snprintf(body, sizeof(body), "{\"type\":\"playback.finished\",\"turn\":\"%s\"}", turn_);
        sendText(body);
        lastActivityAt_ = now;
        setPhase(Phase::Finishing);
      } else if (now - phaseSince_ > kReplyTimeoutMs * 2) {
        fail("playback stalled", now);
      }
      return;

    case Phase::Finishing:
      if (!client_ || !esp_websocket_client_is_connected(client_) ||
          now - phaseSince_ > kCompleteTimeoutMs) {
        failures_ = 0;
        nextTurn(now);
      }
      return;

    case Phase::Off:
      return;
  }
}

void Voice::handle(const Event &event, uint32_t now) {
  const Phase phase = phase_;
  switch (event.kind) {
    case EventKind::Opened:
      Serial.println("[voice] socket open");
      return;

    case EventKind::Ready:
      if (phase != Phase::Connecting) return;
      socketReady_ = true;
      // Connecting is Bunty's wait, not the user's silence.
      lastActivityAt_ += now - connectingSince_;
      logPrintf("[voice] session ready; streaming (%u ms buffered)\n",
                unsigned(microphone_->buffered() / 32));
      setPhase(Phase::Listening);
      return;

    case EventKind::ReadyUnsupported:
      endConversation("gateway lacks turn detection", true);
      return;

    case EventKind::Speech:
      lastActivityAt_ = now;
      return;

    case EventKind::InputEnded:
      if (phase != Phase::Listening) return;
      microphone_->setCapturing(false);
      failures_ = 0;
      setPhase(Phase::Thinking);
      return;

    case EventKind::AudioStart:
      if (phase == Phase::Listening) microphone_->setCapturing(false);
      if (phase == Phase::Listening || phase == Phase::Thinking) setPhase(Phase::Speaking);
      return;

    case EventKind::AudioEnd:
      return;

    case EventKind::Complete:
      if (phase == Phase::Speaking) return;  // the ack is still owed
      failures_ = 0;
      nextTurn(now);
      return;

    case EventKind::Error:
      logPrintf("[voice] gateway error %s\n", event.code);
      if (strcmp(event.code, "rate_limited") == 0 ||
          strcmp(event.code, "ai_budget_or_rate_limit") == 0) {
        endConversation("Rate limited", true);
      } else if (strcmp(event.code, "no_speech") == 0 ||
                 strcmp(event.code, "empty_audio") == 0 ||
                 strcmp(event.code, "playback_ack_timeout") == 0 ||
                 strcmp(event.code, "unexpected_playback_ack") == 0) {
        // The gateway closes the turn itself; the next one opens on Closed.
      } else if (strcmp(event.code, "session_expired") == 0 ||
                 strcmp(event.code, "turn_limit_reached") == 0) {
        nextTurn(now);
      } else {
        fail(event.code, now);
      }
      return;

    case EventKind::Closed:
      if (phase == Phase::Speaking) {
        // Keep playing what already arrived; the ack simply has no socket.
        socketReady_ = false;
        return;
      }
      if (phase == Phase::Finishing || (phase == Phase::Listening && socketReady_ &&
                                        now - phaseSince_ > 1000)) {
        failures_ = phase == Phase::Finishing ? 0 : failures_;
        nextTurn(now);
      } else if (phase != Phase::Off) {
        fail("socket closed", now);
      }
      return;
  }
}

void Voice::trackLocalSpeech(uint32_t now) {
  const uint32_t level = microphone_->level();
  const uint32_t threshold = noiseFloor_ * 3 > 300 ? noiseFloor_ * 3 : 300;
  if (level > threshold) {
    if (lastLocalSpeechAt_ != 0 && now - lastLocalSpeechAt_ < kLocalSpeechHoldMs &&
        !speechPending_) {
      // Two loud frames in a row: a voice, not a single knock or click.
      speechPending_ = true;
      logPrintf("[voice] speech heard (level %u over %u); opening socket\n",
                unsigned(level), unsigned(threshold));
    }
    lastLocalSpeechAt_ = now;
  } else {
    noiseFloor_ = (noiseFloor_ * 63 + level) / 64;
  }
}

bool Voice::silenceExpired(uint32_t now) const {
  const uint32_t quiet = now - lastActivityAt_;
  if (quiet < kSilenceExitMs) return false;
  const bool localSpeech =
      lastLocalSpeechAt_ != 0 && now - lastLocalSpeechAt_ < kLocalSpeechHoldMs;
  return !localSpeech || quiet >= kSilenceExitMs + kLocalSpeechGraceMs;
}

void Voice::upload() {
  if (!client_ || !socketReady_) return;
  // Bounded per pass so gateway events are never starved by a backlog.
  for (int frames = 0; frames < 8; ++frames) {
    if (microphone_->buffered() < kUploadFrameBytes) return;
    size_t filled = 0;
    while (filled < kUploadFrameBytes) {
      const size_t got = microphone_->read(frame_ + filled, kUploadFrameBytes - filled, 0);
      if (got == 0) break;
      filled += got;
    }
    if (filled != kUploadFrameBytes) return;
    const int sent = esp_websocket_client_send_bin(
        client_, reinterpret_cast<const char *>(frame_), int(filled), pdMS_TO_TICKS(kSendTimeoutMs));
    if (sent != int(filled)) {
      logPrintf("[voice] send_bin returned %d of %u (connected=%d, backlog=%u)\n", sent,
                unsigned(filled), int(esp_websocket_client_is_connected(client_)),
                unsigned(microphone_->buffered()));
      fail("audio upload stalled", millis());
      return;
    }
  }
}

// -------------------------------------------------------------- socket ---

bool Voice::openSocket() {
  closeSocket(false);
  textUsed_ = 0;
  lastOpcode_ = 0;
  heard_[0] = '\0';
  playing_ = false;
  audioEnded_ = false;
  drained_ = false;

  esp_websocket_client_config_t config = {};
  config.uri = uri_;
  config.headers = headers_;
  config.use_global_ca_store = true;
  config.disable_auto_reconnect = true;
  config.buffer_size = kSocketBufferBytes;
  config.task_stack = kSocketTaskStack;
  config.task_prio = 5;
  config.ping_interval_sec = 10;
  config.pingpong_timeout_sec = 25;
  client_ = esp_websocket_client_init(&config);
  if (!client_) return false;
  esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, socketEvent, this);
  if (esp_websocket_client_start(client_) != ESP_OK) {
    esp_websocket_client_destroy(client_);
    client_ = nullptr;
    return false;
  }
  Serial.println("[voice] opening gateway socket");
  return true;
}

void Voice::closeSocket(bool sayGoodbye) {
  playing_ = false;
  socketReady_ = false;
  if (client_) {
    if (sayGoodbye && esp_websocket_client_is_connected(client_)) {
      sendText("{\"type\":\"session.close\"}");
    }
    esp_websocket_client_stop(client_);
    esp_websocket_client_destroy(client_);
    client_ = nullptr;
  }
  // Only now can no further events or PCM arrive from the old socket, so
  // everything stamped with the old generation is safely ignored.
  ++generation_;
  flush_ = true;
}

bool Voice::sendText(const char *text) {
  if (!client_ || !esp_websocket_client_is_connected(client_)) return false;
  const int length = int(strlen(text));
  return esp_websocket_client_send_text(client_, text, length, pdMS_TO_TICKS(1000)) == length;
}

void Voice::post(EventKind kind, const char *code) {
  Event event{kind, generation_, {}};
  copyCode(event.code, sizeof(event.code), code);
  if (xQueueSend(events_, &event, pdMS_TO_TICKS(50)) != pdTRUE) {
    Serial.println("[voice] event queue full");
  }
}

// Runs on the IDF WebSocket task. It never stops or destroys the client, which
// would deadlock; it only parses, forwards PCM, and posts events.
void Voice::socketEvent(void *handler, esp_event_base_t, int32_t id, void *data) {
  auto *voice = static_cast<Voice *>(handler);
  auto *event = static_cast<esp_websocket_event_data_t *>(data);
  switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
      voice->post(EventKind::Opened);
      return;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
      voice->post(EventKind::Closed);
      return;
    case WEBSOCKET_EVENT_DATA:
      break;
    default:
      return;
  }
  if (!event || event->data_len < 0) return;

  uint8_t opcode = event->op_code;
  if (opcode == kOpContinuation) opcode = voice->lastOpcode_;
  if (opcode == kOpBinary) {
    voice->lastOpcode_ = kOpBinary;
    voice->onAudio(reinterpret_cast<const uint8_t *>(event->data_ptr), event->data_len);
    return;
  }
  if (opcode != kOpText) return;  // ping, pong, close
  voice->lastOpcode_ = kOpText;

  // IDF delivers a frame larger than buffer_size as several events.
  if (event->payload_offset == 0) voice->textUsed_ = 0;
  const size_t total = event->payload_len;
  if (total > kControlBytes || size_t(event->payload_offset) != voice->textUsed_ ||
      voice->textUsed_ + event->data_len > total) {
    voice->textUsed_ = 0;
    return;
  }
  memcpy(voice->text_ + voice->textUsed_, event->data_ptr, event->data_len);
  voice->textUsed_ += event->data_len;
  if (voice->textUsed_ < total) return;
  voice->text_[voice->textUsed_] = '\0';
  voice->onText(voice->text_, voice->textUsed_);
  voice->textUsed_ = 0;
}

void Voice::onText(const char *text, size_t length) {
  arena_.reset();
  JsonDocument document(&arena_);
  if (deserializeJson(document, text, length) != DeserializationError::Ok) return;
  const char *type = document["type"] | "";
  const char *turn = document["turn"] | "";
  if (*turn) copyCode(turn_, sizeof(turn_), turn);

  if (strcmp(type, "session.ready") == 0) {
    const bool gatewayTurns =
        strcmp(document["turn_detection"] | "", "smart-turn-v2") == 0;
    post(gatewayTurns ? EventKind::Ready : EventKind::ReadyUnsupported);
  } else if (strcmp(type, "speech.started") == 0 || strcmp(type, "speech.resumed") == 0) {
    post(EventKind::Speech);
  } else if (strcmp(type, "transcript.partial") == 0) {
    // Only a transcript that is still growing counts as someone talking; a
    // stuck partial on a humming fan must not hold the conversation open.
    const char *heard = document["text"] | "";
    char prefix[sizeof(heard_)];
    copyCode(prefix, sizeof(prefix), heard);
    if (strcmp(prefix, heard_) != 0) {
      strcpy(heard_, prefix);
      post(EventKind::Speech);
    }
  } else if (strcmp(type, "transcript.final") == 0) {
    logPrintf("[voice] heard: %s\n", document["text"] | "");
    post(EventKind::Speech);
    post(EventKind::InputEnded);
  } else if (strcmp(type, "input.ended") == 0 || strcmp(type, "input.pause") == 0) {
    post(EventKind::InputEnded);
  } else if (strcmp(type, "audio.start") == 0) {
    // Set here, before the first PCM frame is handed to onAudio.
    audioEnded_ = false;
    drained_ = false;
    emotion_ = Emotion::Neutral;
    playing_ = true;
    post(EventKind::AudioStart);
  } else if (strcmp(type, "emotion") == 0) {
    const Cue cue{bytesIn_, parseEmotion(document["name"] | "neutral")};
    if (xQueueSend(cues_, &cue, 0) != pdTRUE) emotion_ = cue.emotion;
    logPrintf("[voice] feeling %s\n", document["name"] | "neutral");
  } else if (strcmp(type, "audio.end") == 0) {
    audioEnded_ = true;
    post(EventKind::AudioEnd);
  } else if (strcmp(type, "assistant.text") == 0) {
    logPrintf("[voice] bunty: %s\n", document["text"] | "");
  } else if (strcmp(type, "session.complete") == 0) {
    post(EventKind::Complete);
  } else if (strcmp(type, "error") == 0) {
    post(EventKind::Error, document["code"] | "unknown");
  }
}

void Voice::onAudio(const uint8_t *data, size_t length) {
  const uint32_t generation = generation_;
  size_t sent = 0;
  // Aura outruns real time; blocking here is TCP backpressure, not loss.
  while (sent < length && playing_ && generation == generation_) {
    const size_t chunk =
        xStreamBufferSend(playback_, data + sent, length - sent, pdMS_TO_TICKS(100));
    sent += chunk;
    bytesIn_ += chunk;
  }
}

// ------------------------------------------------------------ speak task ---

void Voice::speakTask(void *context) {
  auto *voice = static_cast<Voice *>(context);
  uint32_t sampledAt = 0;
  for (;;) {
    voice->playOnce();
    if (millis() - sampledAt >= 10000) {
      sampledAt = millis();
      memory::sampleTask("speak");
    }
  }
}

void Voice::playOnce() {
  static int16_t samples[kPlaySamples];
  static bool primed = false;

  if (!playing_) {
    if (speaker_.isOpen()) {
      speaker_.close();
      power::release(power::Load::kAudio);
    }
    primed = false;
    outputLevel_ = 0.0f;
    if (flush_.exchange(false)) {
      uint8_t scratch[512];
      size_t dropped;
      while ((dropped = xStreamBufferReceive(playback_, scratch, sizeof(scratch), 0)) > 0) {
        bytesOut_ += dropped;
      }
      Cue stale;
      while (xQueueReceive(cues_, &stale, 0) == pdTRUE) {
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }

  if (!speaker_.isOpen()) {
    power::claim(power::Load::kAudio);
    if (!speaker_.open()) {
      Serial.println("[speaker] could not open; skipping reply");
      playing_ = false;
      drained_ = true;
      return;
    }
  }

  const size_t available = xStreamBufferBytesAvailable(playback_);
  if (!primed) {
    if (available >= cfg::audio::kPrebufferBytes || (audioEnded_ && available > 1)) {
      primed = true;
    } else if (audioEnded_) {
      vTaskDelay(pdMS_TO_TICKS(cfg::audio::kSpeakerDrainMs));
      speaker_.close();
      power::release(power::Load::kAudio);
      outputLevel_ = 0.0f;
      playing_ = false;
      drained_ = true;
      return;
    } else {
      outputLevel_ = outputLevel_ * 0.8f;
      vTaskDelay(pdMS_TO_TICKS(5));
      return;
    }
  }

  auto *bytes = reinterpret_cast<uint8_t *>(samples);
  size_t got = xStreamBufferReceive(playback_, bytes, sizeof(samples), pdMS_TO_TICKS(20));
  if (got & 1) {
    // Chunks arrive at arbitrary byte boundaries; complete the split sample.
    got += xStreamBufferReceive(playback_, bytes + got, 1, pdMS_TO_TICKS(100));
  }
  bytesOut_ += got;
  got &= ~size_t(1);
  if (got == 0) {
    primed = false;  // underrun between sentences: re-cushion before resuming
    return;
  }
  // A cue fires with the chunk its sentence starts in. Compared by
  // difference so the byte counters may wrap.
  Cue cue;
  while (xQueuePeek(cues_, &cue, 0) == pdTRUE &&
         static_cast<int32_t>(cue.position - bytesOut_) < 0) {
    xQueueReceive(cues_, &cue, 0);
    emotion_ = cue.emotion;
  }
  const size_t count = got / 2;
  float energy = 0.0f;
  for (size_t i = 0; i < count; ++i) energy += float(samples[i]) * samples[i];
  outputLevel_ = sqrtf(energy / count) / cfg::audio::kLevelFullScale;
  if (!speaker_.write(samples, count, cfg::audio::kVolumePercent)) {
    Serial.println("[speaker] I2S write failed");
  }
}
