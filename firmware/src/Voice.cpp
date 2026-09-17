#include "Log.h"
#include "Voice.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <esp_tls.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "Config.h"
#include "VoiceCerts.h"
#include "TapInput.h"
#include "InternalAllocation.h"

namespace {

using namespace cfg::voice;

// Root-mean-square amplitude of one capture chunk.
uint32_t frameLevel(const uint8_t *frame) {
  const int16_t *samples = reinterpret_cast<const int16_t *>(frame);
  uint64_t sumSquares = 0;
  for (size_t i = 0; i < kFrameSamples; ++i) {
    const int32_t value = samples[i];
    sumSquares += static_cast<uint64_t>(value) * value;
  }
  return static_cast<uint32_t>(
      sqrtf(static_cast<float>(sumSquares / kFrameSamples)));
}

bool globalCaStoreReady = false;

bool ensureGlobalCaStore() {
  if (globalCaStoreReady) return true;
  if (esp_tls_init_global_ca_store() != ESP_OK) {
    Serial.println("[voice] could not initialize the TLS trust store");
    return false;
  }
  if (esp_tls_set_global_ca_store(
          reinterpret_cast<const unsigned char *>(kIotGatewayRootCertificates),
          strlen(kIotGatewayRootCertificates) + 1) != ESP_OK) {
    Serial.println("[voice] could not load the gateway root certificates");
    return false;
  }
  globalCaStoreReady = true;
  return true;
}

}  // namespace

const char *BuntyVoice::stateName(State state) {
  switch (state) {
    case State::Disabled: return "disabled";
    case State::Offline: return "offline";
    case State::Connecting: return "connecting";
    case State::Listening: return "listening";
    case State::Uploading: return "uploading";
    case State::Thinking: return "thinking";
    case State::Speaking: return "speaking";
    case State::Draining: return "draining";
  }
  return "?";
}

bool BuntyVoice::begin(TapInput *microphone, const char *gatewayHost,
                       const char *deviceId, const char *gatewayToken,
                       const char *sessionId) {
  if (running_) return true;
  if (activeTasks_ > 0) return false;
  if (playbackStorage_) end();
  if (!ESP.getPsramSize()) {
    Serial.println("[voice] unavailable: full voice buffering requires PSRAM");
    return false;
  }

  if (!gatewayHost || gatewayHost[0] == '\0' || !deviceId ||
      deviceId[0] == '\0' || !gatewayToken || gatewayToken[0] == '\0') {
    Serial.println("[voice] no stored gateway credential; activation deferred");
    state_ = State::Disabled;
    return false;
  }
  if (!microphone || !microphone->available()) {
    Serial.println("[voice] microphone unavailable; voice stays disabled");
    state_ = State::Disabled;
    return false;
  }
  if (!ensureGlobalCaStore()) {
    state_ = State::Disabled;
    return false;
  }

  microphone_ = microphone;
  noiseFloorRms_ = kInitialNoiseFloorRms;
  conversation_.close();

  if (snprintf(uri_, sizeof(uri_), "wss://%s/v1/live", gatewayHost) >= int(sizeof(uri_))) return false;
  // esp_websocket_client expects a single CRLF-terminated header block. The
  // request ID is only ever a correlation aid; the gateway derives the device
  // identity from the token alone.
  const int count = snprintf(headers_, sizeof(headers_),
      "Authorization: Bearer %s\r\nX-Request-ID: %s:live\r\nX-Bunty-Turn-Detection: gateway\r\n%s%s%s",
      gatewayToken, deviceId, sessionId && *sessionId ? "X-Bunty-Session: " : "",
      sessionId && *sessionId ? sessionId : "", sessionId && *sessionId ? "\r\n" : "");
  if (count < 0 || count >= int(sizeof(headers_))) return false;
  // Scopes the gateway's short-term conversation memory to this firmware build.
  // Omitted when unset so the gateway simply runs stateless.

  {
    memory::InternalAllocationScope internal;
    if (!vad_.begin()) {
      Serial.println("[voice] could not initialize internal WebRTC VAD");
      end();
      return false;
    }
  }

  // PSRAM keeps the reply ring away from the internal heap the framebuffer,
  // Wi-Fi, and the TLS session already compete for.
  playbackStorage_ = static_cast<uint8_t *>(heap_caps_malloc(
      kPlaybackRingBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (playbackStorage_) {
    playback_ = xStreamBufferCreateStatic(kPlaybackRingBytes + 1, 1,
                                          playbackStorage_, &playbackControl_);
  }
  preRoll_ = static_cast<uint8_t *>(memory::internal(kPreRollFrames * kFrameBytes));
  controlText_ = static_cast<char *>(memory::external(kControlBytes + 1));
  controlMessage_.bind(controlText_, kControlBytes);
  controlArenaStorage_ = static_cast<uint8_t *>(memory::external(32768));
  controlArena_.bind(controlArenaStorage_, 32768);
  if (!playback_ || !preRoll_ || !controlText_ || !controlArenaStorage_ ||
      !decoder_.begin(cfg::audio::kReplyInputBytes)) {
    Serial.println("[voice] could not allocate the audio buffers");
    end();
    return false;
  }

  if (!microphone_->startCapture(kCaptureRingBytes)) {
    Serial.println("[voice] could not start microphone capture");
    end();
    return false;
  }

  running_ = true;
  state_ = WiFi.status() == WL_CONNECTED ? State::Listening : State::Offline;
  socketCleanupRequested_ = false;
  closeSessionRequested_ = false;
  cleanupStarted_ = false;
  connectingSince_ = 0;
  lastSocketActivityAt_ = 0;

  // Network management/upload use core 0; paced decoding/playback uses core 1
  // alongside capture and servo workers, above the display loop's priority.
  ++activeTasks_;
  if (xTaskCreatePinnedToCore(networkTask, "voice-net", 4096, this, 2, nullptr,
                              0) != pdPASS) {
    --activeTasks_;
    end();
    return false;
  }
  ++activeTasks_;
  if (xTaskCreatePinnedToCore(uploadTask, "voice-up", 4096, this, 4, nullptr,
                              0) != pdPASS) {
    --activeTasks_;
    Serial.println("[voice] could not start the upload task");
    end();
    return false;
  }
  ++activeTasks_;
  if (xTaskCreatePinnedToCore(playbackTask, "voice-play", 4096, this, 5,
                              nullptr, 1) != pdPASS) {
    --activeTasks_;
    Serial.println("[voice] could not start the playback task");
    end();
    return false;
  }

  logPrintf("[voice] transport ready for %s as %s\n", uri_,
                deviceId);
  logPrintf("[voice] WebRTC VAD mode=%d frame=%u ms onset=%u ms preroll=%u ms no-speech=%u ms; gateway endpoint; tap to arm\n",
      kVadMode, unsigned(kFrameDurationMs), unsigned(kSpeechStartMs),
      unsigned(kPreRollFrames * kFrameDurationMs), unsigned(kNoSpeechTimeoutMs));
  logPrintf("[memory] voice psram capture=%u playback=%u compressed=%u control=%u arena=%u; internal preroll=%u pcm=4608 upload=%u\n",
      unsigned(kCaptureRingBytes + 1), unsigned(kPlaybackRingBytes + 1), unsigned(cfg::audio::kReplyInputBytes),
      unsigned(kControlBytes + 1), 32768u, unsigned(kPreRollFrames * kFrameBytes), unsigned(sizeof(uploadFrame_)));
  return true;
}

void BuntyVoice::end() {
  running_ = false;
  conversation_.close();

  // Wait for both tasks to actually retire. Each lowers the count as its last
  // act, so this joins them rather than guessing at a delay and freeing the
  // ring buffers while a task is still blocked inside one.
  const uint32_t deadline = millis() + 2000;
  while (activeTasks_ > 0 && static_cast<int32_t>(millis() - deadline) < 0) {
    delay(10);
  }
  if (activeTasks_ > 0) {
    Serial.println("[voice] audio tasks did not retire; buffers retained");
    return;
  }

  if (client_) {
    esp_websocket_client_stop(client_);
    esp_websocket_client_destroy(client_);
    client_ = nullptr;
  }

  speaker_.end();
  if (microphone_) microphone_->stopCapture();
  if (playback_) {
    vStreamBufferDelete(playback_);
    playback_ = nullptr;
  }
  free(playbackStorage_);
  playbackStorage_ = nullptr;
  free(preRoll_);
  preRoll_ = nullptr;
  free(controlText_);
  controlText_ = nullptr;
  free(controlArenaStorage_);
  controlArenaStorage_ = nullptr;
  decoder_.end();
  vad_.end();

  socketCleanupRequested_ = false;
  connectingSince_ = 0;
  state_ = State::Disabled;
}

void BuntyVoice::arm() {
  if (!running_) return;
  const bool wasArmed = armed();
  conversation_.arm(millis());
  if (!wasArmed) resetGateRequested_ = true;
  // Keep the socket closed until actual speech begins. This is the privacy and
  // noise boundary; pre-roll retains the lead-in, so detection does not clip
  // the first word.
  if (state_ == State::Listening) {
    Serial.println("[voice] armed by tap; waiting for speech");
  }
}

bool BuntyVoice::engaged() const {
  const State state = state_;
  return state == State::Connecting || state == State::Uploading ||
         state == State::Thinking || state == State::Speaking || state == State::Draining;
}

void BuntyVoice::networkTask(void *context) {
  auto *voice = static_cast<BuntyVoice *>(context);
  while (voice->running_) {
    voice->serviceNetwork();
    memory::sampleTask("voice-net");
    delay(10);
  }
  --voice->activeTasks_;
  vTaskDelete(nullptr);
}

void BuntyVoice::serviceNetwork() {
  if (!running_) return;

  // Drop the arm once the window lapses, but never mid-turn.
  if ((state_ == State::Listening || state_ == State::Offline) &&
      conversation_.expire(millis())) {
    resetGateRequested_ = true;
    Serial.println("[voice] no speech for 4000 ms; session closed, tap to talk again");
  }

  // Transport teardown is driven from voice-net, never from the WebSocket event
  // task where esp_websocket_client_stop() would deadlock.
  if (socketCleanupRequested_) {
    state_ = State::Offline;
    if (!cleanupStarted_) {
      cleanupStarted_ = true;
      ++cleanupEpoch_;
    }
    const uint32_t epoch = cleanupEpoch_;
    if (playbackEpoch_ != epoch || uploadEpoch_ != epoch) return;
    // The uploader has quiesced, so session.close cannot overtake one of its
    // PCM sends. Never stop/send from inside the WebSocket callback task.
    if (closeSessionRequested_.exchange(false) && client_ &&
        esp_websocket_client_is_connected(client_)) {
      sendJson("{\"type\":\"session.close\"}");
    }
    if (client_) esp_websocket_client_stop(client_);
    // Both the producer (socket task) and consumer have now quiesced.
    if (playback_) xStreamBufferReset(playback_);
    connectingSince_ = 0;
    lastSocketActivityAt_ = 0;
    audioEndSeen_ = false;
    acknowledgePending_ = false;
    finishPending_ = false;
    resetGateRequested_ = true;
    if (WiFi.status() == WL_CONNECTED) {
      state_ = State::Listening;
      if (armed()) conversation_.arm(millis());
      Serial.println(armed() ? "[voice] turn complete; 4 s follow-up window"
                             : "[voice] turn complete; tap to talk again");
    }
    cleanupStarted_ = false;
    socketCleanupRequested_ = false;
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (state_ != State::Offline) {
      Serial.println("[voice] Wi-Fi unavailable; suspending voice");
      if (engaged()) {
        requestSocketCleanup();
      } else {
        state_ = State::Offline;
      }
    }
    return;
  }

  if (state_ == State::Offline) {
    resetGateRequested_ = true;
    state_ = State::Listening;
    Serial.println("[voice] microphone capture ready; cloud socket closed");
    return;
  }

  if (state_ == State::Uploading) {
    const auto reason = inputWatchdog_.reason(millis());
    if (reason != InputSpeechWatchdog::Reason::None) {
      abortInput(reason == InputSpeechWatchdog::Reason::LocalNonSpeech
                     ? "4000 ms without confirmed local speech"
                     : "4000 ms without gateway transcript progress");
      return;
    }
  }

  // A valid TLS connection can remain alive on ping/pong while the actual
  // conversation has stalled. Bound only application silence: microphone PCM,
  // control/audio frames, and audible reply playback refresh the timestamp.
  if (client_ && esp_websocket_client_is_connected(client_) && engaged() &&
      lastSocketActivityAt_ != 0 &&
      millis() - lastSocketActivityAt_ >= kSocketIdleTimeoutMs) {
    logPrintf("[voice] no application audio or control for %u ms; closing turn\n",
                  static_cast<unsigned>(kSocketIdleTimeoutMs));
    requestSocketCleanup();
    return;
  }
  if (finishPending_.exchange(false)) sendPlaybackFinished();
  if (state_ != State::Connecting) return;

  // Do not attempt certificate validation against the ESP32's boot-time clock.
  // main.cpp requests NTP asynchronously once Wi-Fi is available.
  if (time(nullptr) < cfg::net::kMinimumTrustedUnixTime) {
    static uint32_t nextLogAt = 0;
    if (static_cast<int32_t>(millis() - nextLogAt) >= 0) {
      nextLogAt = millis() + 5000;
      Serial.println("[voice] waiting for trusted clock before TLS");
    }
    return;
  }

  if (connectingSince_ != 0) {
    if (millis() - connectingSince_ > kConnectTimeoutMs) {
      Serial.println("[voice] no session.ready within timeout; turn aborted");
      requestSocketCleanup();
    }
    return;
  }
  connectingSince_ = millis();
  if (!openTurnSocket()) requestSocketCleanup();
}

bool BuntyVoice::openTurnSocket() {
  lastSocketActivityAt_ = millis();
  if (client_) {
    // Each utterance gets a fresh WebSocket session, but the stopped handle is
    // reusable and avoids heap churn in esp-tls.
    if (esp_websocket_client_start(client_) != ESP_OK) {
      Serial.println("[voice] could not reopen the gateway WebSocket");
      return false;
    }
    Serial.println("[voice] speech buffered; opening gateway socket");
    return true;
  }

  esp_websocket_client_config_t config = {};
  config.uri = uri_;
  config.headers = headers_;
  config.use_global_ca_store = true;
  // An utterance owns exactly one session; retries require fresh local speech.
  config.disable_auto_reconnect = true;
  config.buffer_size = kSocketBufferBytes;
  config.task_stack = kSocketTaskStack;
  config.task_prio = kSocketTaskPriority;
  config.ping_interval_sec = kPingIntervalSec;
  config.pingpong_timeout_sec = kPingTimeoutSec;

  client_ = esp_websocket_client_init(&config);
  if (!client_) {
    Serial.println("[voice] could not initialize the WebSocket client");
    return false;
  }
  esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, websocketEvent,
                                this);
  if (esp_websocket_client_start(client_) != ESP_OK) {
    Serial.println("[voice] could not open the gateway WebSocket");
    esp_websocket_client_destroy(client_);
    client_ = nullptr;
    return false;
  }
  logPrintf("[voice] speech buffered; opening %s\n", uri_);
  return true;
}

void BuntyVoice::requestSocketCleanup() {
  if (state_ == State::Disabled) return;
  socketCleanupRequested_ = true;
  state_ = State::Offline;
}

void BuntyVoice::abortInput(const char *reason) {
  // A concurrently accepted gateway endpoint wins: never cancel Thinking or
  // Speaking just because input has (correctly) stopped during the reply.
  State expected = State::Uploading;
  if (!state_.compare_exchange_strong(expected, State::Offline)) return;
  conversation_.close();
  closeSessionRequested_ = true;
  socketCleanupRequested_ = true;
  logPrintf("[voice] %s; closing session, tap to talk again\n", reason);
}

void BuntyVoice::websocketEvent(void *handler, esp_event_base_t base,
                                int32_t eventId, void *eventData) {
  auto *voice = static_cast<BuntyVoice *>(handler);
  auto *data = static_cast<esp_websocket_event_data_t *>(eventData);
  if (!voice || !voice->running_ || voice->socketCleanupRequested_) return;
  memory::sampleTask("websocket");

  switch (eventId) {
    case WEBSOCKET_EVENT_CONNECTED:
      voice->controlMessage_.reset();
      Serial.println("[voice] gateway socket open");
      // State stays Connecting until session.ready. The capture ring continues
      // to retain speech recorded during this handshake.
      voice->state_ = State::Connecting;
      voice->lastSocketActivityAt_ = millis();
      break;
    case WEBSOCKET_EVENT_DATA:
      if (!data || data->data_len <= 0) break;
      voice->lastSocketActivityAt_ = millis();
      switch (voice->controlMessage_.append(data->op_code, data->data_ptr,
          data->data_len, data->payload_len, data->payload_offset)) {
        case ControlMessage::Result::Audio:
          voice->handleAudio(reinterpret_cast<const uint8_t *>(data->data_ptr), data->data_len);
          break;
        case ControlMessage::Result::Text:
          voice->handleControl(voice->controlText_, voice->controlMessage_.size());
          break;
        case ControlMessage::Result::Invalid:
          voice->requestSocketCleanup();
          break;
        default: break;
      }
      break;
    case WEBSOCKET_EVENT_ERROR:
      // Usually TLS: a rejected certificate chain or a refused handshake. Raise
      // CORE_DEBUG_LEVEL to see esp-tls report the specific cause.
      Serial.println("[voice] websocket transport error");
      voice->onDisconnected();
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
      Serial.println("[voice] websocket closed");
      voice->onDisconnected();
      break;
    default:
      break;
  }
}

void BuntyVoice::onDisconnected() {
  if (state_ == State::Disabled || state_ == State::Offline ||
      state_ == State::Listening || socketCleanupRequested_) {
    return;
  }
  requestSocketCleanup();
}

void BuntyVoice::handleControl(char *payload, size_t length) {
  controlArena_.reset();
  JsonDocument document(&controlArena_);
  if (deserializeJson(document, payload, length) != DeserializationError::Ok) {
    return;
  }
  const char *type = document["type"] | "";
  if (document["turn"].is<uint32_t>()) turn_ = document["turn"].as<uint32_t>();
  if (state_ == State::Connecting) {
    // Anything arriving before session.ready is worth seeing; a refusal shows
    // up here as an error frame rather than a silent stall.
    logPrintf("[voice] pre-session control: %s\n", type);
  }

  if (strcmp(type, "session.ready") == 0) {
    if (state_ != State::Connecting) return;
    if (strcmp(document["turn_detection"] | "", "smart-turn-v2") != 0) {
      conversation_.close();
      closeSessionRequested_ = true;
      requestSocketCleanup();
      Serial.println("[voice] gateway lacks negotiated turn detection; update gateway before using this firmware");
      return;
    }
    connectingSince_ = 0;
    inputWatchdog_.begin(millis());
    // Keep the confirmed onset: a short word may exist entirely in pre-roll.
    State expected = State::Connecting;
    if (!state_.compare_exchange_strong(expected, State::Uploading)) return;
    Serial.println("[voice] session ready; streaming all captured audio");
  } else if (strcmp(type, "transcript.partial") == 0 || strcmp(type, "transcript.final") == 0) {
    const char *text = document["text"] | "";
    inputWatchdog_.observeTranscript(millis(), text);
    if (strcmp(type, "transcript.final") == 0) logPrintf("[voice] heard: %s\n", text);
  } else if (gatewayEndsInput(type)) {
    // Stop uploading immediately. Everything the microphone records from here
    // until the turn ends is Bunty's own voice.
    State expected = State::Uploading;
    state_.compare_exchange_strong(expected, State::Thinking);
  } else if (strcmp(type, "speech.started") == 0 || strcmp(type, "speech.resumed") == 0 ||
             strcmp(type, "speech.maybe_finished") == 0 || strcmp(type, "turn_detection") == 0) {
    // Candidate/incomplete/resumed events are advisory, not endpoints. Keep
    // streaming until input.ended; these events alone do not prove real words.
    logPrintf("[voice] gateway %s; awaiting confirmed endpoint\n", type);
  } else if (strcmp(type, "assistant.text") == 0) {
    logPrintf("[voice] reply: %s\n",
                  static_cast<const char *>(document["text"] | ""));
  } else if (strcmp(type, "audio.start") == 0) {
    if (state_ != State::Thinking) return;
    audioEndSeen_ = false;
    acknowledgePending_ = true;
    speakingSince_ = millis();
    State expected = State::Thinking;
    state_.compare_exchange_strong(expected, State::Speaking);
  } else if (strcmp(type, "audio.end") == 0) {
    // The playback task owes the acknowledgement once the ring and the DMA have
    // both emptied.
    audioEndSeen_ = true;
  } else if (strcmp(type, "session.complete") == 0) {
    Serial.println("[voice] turn complete");
    requestSocketCleanup();
  } else if (strcmp(type, "input.resume") == 0) {
    // Compatibility with an older gateway: this firmware uses one socket per
    // utterance, so a resume means that turn is complete and the socket ends.
    requestSocketCleanup();
  } else if (strcmp(type, "error") == 0) {
    if (strcmp(document["code"] | "", "no_speech") == 0) conversation_.close();
    logPrintf("[voice] gateway error %s\n",
                  static_cast<const char *>(document["code"] | "unknown"));
    requestSocketCleanup();
  }
}

void BuntyVoice::handleAudio(const uint8_t *data, size_t length) {
  if (!playback_ || !data || length == 0 || state_ != State::Speaking) return;
  // A short timeout rather than a drop: the decoder and speaker consume at a
  // fixed rate, so brief backpressure is normal and discarding clips a reply.
  if (xStreamBufferSend(playback_, data, length, pdMS_TO_TICKS(200)) != length) {
    Serial.println("[audio] playback ring overflow; aborting incomplete reply");
    requestSocketCleanup();
  }
}

bool BuntyVoice::sendJson(const char *body) {
  if (!client_ || !esp_websocket_client_is_connected(client_)) {
    Serial.println("[voice] control not sent: socket is no longer connected");
    requestSocketCleanup();
    return false;
  }
  const int length = static_cast<int>(strlen(body));
  const int written =
      esp_websocket_client_send_text(client_, body, length, pdMS_TO_TICKS(1000));
  if (written == length) {
    lastSocketActivityAt_ = millis();
    return true;
  }

  // A zero result means the transport's write poll timed out; -1 is a socket
  // error. Either way this session cannot safely advance to the next state.
  logPrintf("[voice] control write stalled (%d/%d); closing turn\n", written,
                length);
  requestSocketCleanup();
  return false;
}

void BuntyVoice::sendPlaybackFinished() {
  char body[96];
  snprintf(body, sizeof(body), "{\"type\":\"playback.finished\",\"turn\":\"%lu\"}",
           (unsigned long)turn_.load());
  sendJson(body);
}

bool BuntyVoice::sendFrame(const uint8_t *frame) {
  if (state_ != State::Uploading || socketCleanupRequested_) return false;
  if (!client_ || !esp_websocket_client_is_connected(client_)) {
    Serial.println("[voice] PCM not sent: socket is no longer connected");
    requestSocketCleanup();
    return false;
  }
  const int written = esp_websocket_client_send_bin(
      client_, reinterpret_cast<const char *>(frame),
      static_cast<int>(kFrameBytes), pdMS_TO_TICKS(1000));
  if (written == static_cast<int>(kFrameBytes)) {
    lastSocketActivityAt_ = millis();
    return true;
  }

  logPrintf("[voice] PCM write stalled (%d/%u); closing turn\n", written,
                static_cast<unsigned>(kFrameBytes));
  requestSocketCleanup();
  return false;
}

void BuntyVoice::pushPreRoll(const uint8_t *frame) {
  if (!preRoll_) return;
  memcpy(preRoll_ + preRollNext_ * kFrameBytes, frame, kFrameBytes);
  preRollNext_ = (preRollNext_ + 1) % kPreRollFrames;
  if (preRollFrames_ < kPreRollFrames) ++preRollFrames_;
}

void BuntyVoice::flushPreRoll() {
  if (!preRoll_) return;
  // Oldest first. Until the ring has wrapped the oldest frame is at index 0;
  // afterwards it is wherever the next write would land.
  const size_t start = preRollFrames_ == kPreRollFrames ? preRollNext_ : 0;
  for (size_t i = 0; i < preRollFrames_; ++i) {
    if (!sendFrame(preRoll_ + ((start + i) % kPreRollFrames) * kFrameBytes)) {
      break;
    }
  }
  preRollFrames_ = 0;
  preRollNext_ = 0;
}

void BuntyVoice::resetGate() {
  preRollFrames_ = 0;
  preRollNext_ = 0;
  gate_.reset();
  vad_.reset();
  vadSpeech_ = false;
}

void BuntyVoice::updateNoiseFloor(uint32_t level) {
  // Diagnostic only; caller excludes WebRTC speech. This no longer raises an
  // RMS threshold until quieter speech becomes undetectable.
  noiseFloorRms_ = (noiseFloorRms_ * 15 + level) / 16;
}

void BuntyVoice::uploadTask(void *context) {
  auto *voice = static_cast<BuntyVoice *>(context);
  uint8_t *frame = voice->uploadFrame_;

  bool vadWasArmed = false;
  bool streamStarted = false;
  size_t filled = 0;
  while (voice->running_) {
    voice->uploadIdle_ = true;
    memory::sampleTask("voice-up");
    if (voice->socketCleanupRequested_) {
      voice->uploadEpoch_ = voice->cleanupEpoch_.load();
      vadWasArmed = false;
      streamStarted = false;
      filled = 0;
      delay(10);
      continue;
    }
    if (voice->resetGateRequested_.exchange(false)) {
      voice->resetGate();
      filled = 0;
    }
    const State state = voice->state_;

    if (state == State::Listening && !vadWasArmed) {
      // Audio captured during the previous reply is speaker echo. Discard it
      // before locally arming for the next independent utterance.
      uint8_t scratch[512];
      while (voice->microphone_->readCapture(scratch, sizeof(scratch), 0) > 0) {
      }
      voice->resetGate();
      vadWasArmed = true;
      streamStarted = false;
      filled = 0;
    }

    if (state == State::Connecting) {
      // TapInput keeps filling its large PSRAM capture ring while TLS and the
      // gateway's Flux connection open. Do not consume those samples yet.
      vadWasArmed = false;
      delay(10);
      continue;
    }

    if (state != State::Listening && state != State::Uploading) {
      vadWasArmed = false;
      streamStarted = false;
      filled = 0;
      delay(20);
      continue;
    }

    if (state == State::Uploading && !streamStarted) {
      voice->uploadIdle_ = false;
      // The capture ring holds every frame collected while TLS and Flux
      // connected, beginning at the user's tap. No energy threshold decides
      // whether those opening words are sent.
      voice->flushPreRoll();
      if (voice->state_ != State::Uploading) continue;
      streamStarted = true;
    }

    while (filled < kFrameBytes && voice->state_ == state && voice->running_) {
      const size_t received = voice->microphone_->readCapture(
          frame + filled, kFrameBytes - filled, 250);
      if (received == 0) break;
      filled += received;
    }
    if (filled < kFrameBytes || voice->state_ != state) continue;
    filled = 0;

    const uint32_t level = frameLevel(frame);
    voice->vadLevel_ = level;
    voice->vadThreshold_ = kMinimumSpeechRms;
    const int decision = voice->vad_.process(
        reinterpret_cast<const int16_t *>(frame), kFrameSamples);
    if (decision < 0) {
      voice->conversation_.close();
      voice->requestSocketCleanup();
      Serial.println("[voice] invalid VAD frame; turn aborted");
      continue;
    }
    const bool speech = decision == 1 && level >= kMinimumSpeechRms;
    voice->vadSpeech_ = speech;

    if (state == State::Listening) {
      if (decision == 0) voice->updateNoiseFloor(level);
      if (!voice->armed()) continue;

      // The armed window stays local until actual speech begins. Retaining the
      // lead-in prevents the onset detector from clipping the first word.
      voice->pushPreRoll(frame);
      voice->gate_.observe(speech);
      if (voice->gate_.started()) {
        voice->connectingSince_ = 0;
        State expected = State::Listening;
        if (!voice->armed() || !voice->state_.compare_exchange_strong(expected, State::Connecting)) continue;
        Serial.println("[voice] speech detected; opening live conversation");
      }
      continue;
    }

    voice->gate_.observe(speech);
    voice->inputWatchdog_.observeLocal(millis(), voice->gate_.confirmed());

    if (voice->gate_.timedOut()) {
      voice->abortInput("4000 ms of non-speech PCM (noise counts as non-speech)");
      continue;
    }

    // Include every frame, including silence/soft syllables rejected by VAD.
    // Count audio duration, not upload wall time: TLS backlog drains in bursts.
    voice->uploadIdle_ = false;
    if (!voice->sendFrame(frame)) continue;

    // No local input.end: Flux/Smart Turn own the conversational endpoint.
  }

  voice->uploadIdle_ = true;
  --voice->activeTasks_;
  vTaskDelete(nullptr);
}

void BuntyVoice::playbackTask(void *context) {
  auto *voice = static_cast<BuntyVoice *>(context);
  Mp3Stream &stream = voice->decoder_;

  while (voice->running_) {
    memory::sampleTask("voice-play");
    if (voice->socketCleanupRequested_) {
      voice->speaker_.end();
      stream.discardInput();
      voice->speechLevel_ = 0;
      voice->playbackIdle_ = true;
      voice->playbackEpoch_ = voice->cleanupEpoch_.load();
      delay(10);
      continue;
    }
    if (voice->state_ != State::Speaking) {
      voice->speaker_.end();
      stream.discardInput();
      voice->speechLevel_ = 0;
      voice->playbackIdle_ = true;
      delay(10);
      continue;
    }
    voice->playbackIdle_ = false;
    if (!voice->speaker_.begin(cfg::audio::kMicrophoneSampleRate)) {
      voice->requestSocketCleanup();
      continue;
    }
    size_t received = 0;
    if (voice->playback_ && stream.writable() > 0) {
      received = xStreamBufferReceive(voice->playback_, stream.writePointer(),
                                      stream.writable(), pdMS_TO_TICKS(40));
      stream.commit(received);
    }

    Mp3Stream::Frame frame;
    while (true) {
      if (voice->state_ != State::Speaking || !voice->running_) break;
      const Mp3Stream::Status status = stream.decode(&frame);
      if (status == Mp3Stream::Status::NeedMoreData) break;
      if (status == Mp3Stream::Status::Skipped) continue;

      if (!voice->speaker_.setSampleRate(frame.sampleRate)) {
        voice->requestSocketCleanup();
        break;
      }
      // The mouth opens on the audio actually reaching the amplifier rather
      // than on a timer, smoothed so it does not chatter between frames.
      uint32_t openness =
          meanMagnitude(frame.samples, frame.frames, frame.channels) * 100 /
          2500;
      if (openness > 100) openness = 100;
      voice->speechLevel_ = static_cast<uint8_t>(
          (static_cast<uint32_t>(voice->speechLevel_) * 2 + openness) / 3);

      if (!voice->speaker_.write(frame.samples, frame.frames, frame.channels,
                            cfg::audio::kSpeakingVolumePercent)) {
        voice->requestSocketCleanup();
        break;
      }
      voice->lastSocketActivityAt_ = millis();
    }

    const bool sourceEmpty =
        !voice->playback_ || xStreamBufferBytesAvailable(voice->playback_) == 0;
    if (voice->audioEndSeen_ && sourceEmpty && received == 0 &&
        stream.buffered() > 0) {
      logPrintf("[voice] discarding %u trailing MP3 bytes\n",
                    static_cast<unsigned>(stream.buffered()));
      stream.discardInput();
    }

    // Only acknowledge once the gateway has said the reply is complete;
    // otherwise a momentary network gap would unmute the microphone in the
    // middle of Bunty's sentence.
    if (voice->state_ == State::Speaking && voice->audioEndSeen_ && sourceEmpty && stream.buffered() == 0 &&
        voice->acknowledgePending_) {
      delay(cfg::audio::kDmaDrainMs);
      voice->speaker_.end();
      voice->audioEndSeen_ = false;
      voice->acknowledgePending_ = false;
      State expected = State::Speaking;
      if (!voice->state_.compare_exchange_strong(expected, State::Draining)) continue;
      voice->finishPending_ = true;
      // The conversation window is measured from here -- the moment the reply
      // has physically finished playing -- not from when the turn began.
      if (voice->armed()) voice->conversation_.arm(millis());
      logPrintf("[voice] turn %lu played in %u ms\n",
                    (unsigned long)voice->turn_.load(),
                    static_cast<unsigned>(millis() - voice->speakingSince_));
    }
  }

  voice->speaker_.end();
  voice->playbackIdle_ = true;
  --voice->activeTasks_;
  vTaskDelete(nullptr);
}
