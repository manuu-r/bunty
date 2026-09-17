#pragma once

#include <Arduino.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

#include "Audio.h"
#include "Memory.h"
#include "ControlMessage.h"
#include "LocalVad.h"

class TapInput;

#ifndef IOT_GATEWAY_HOST
#define IOT_GATEWAY_HOST "bunty.underdogthinkers.com"
#endif

// Runs voice activity detection locally, opens the IoT Gateway WebSocket only
// for an utterance, streams that utterance, and plays the spoken reply.
//
// The gateway pipeline is half-duplex: Flux/Smart Turn decide the endpoint;
// local VAD only admits speech and closes unproductive input after four seconds.
// Claude answers, and Aura speaks. The gateway closes the turn only after Bunty
// acknowledges that the speaker and DMA buffers have drained.
//
// The microphone is not claimed directly. TapInput owns the capture I2S port
// and remains its only reader, publishing PCM for this class, so tap gestures
// keep working while voice is armed.
class BuntyVoice {
 public:
  enum class State {
    Disabled,    // No credential, or voice was never started.
    Offline,     // Wi-Fi is unavailable, or a socket is being cleaned up.
    Listening,   // Local VAD is armed; there is no cloud connection.
    Connecting,  // Speech found; buffering while the socket opens.
    Uploading,   // Streaming the active utterance to Flux.
    Thinking,    // Turn finalized; Flux, Claude, or Aura is working.
    Speaking,    // Receiving and playing the reply.
    Draining,    // Playback finished locally, acknowledgement pending.
  };

  bool begin(TapInput *microphone, const char *gatewayHost,
             const char *deviceId, const char *gatewayToken,
             const char *sessionId);
  void end();

  // Arm local VAD for a short conversation window. Until armed, room noise is
  // ignored and no cloud socket is ever opened, so a tap is required to talk.
  // Each completed reply extends the window; it lapses after silence.
  void arm();
  bool armed() const { return conversation_.armed(); }

  State state() const { return state_; }
  static const char *stateName(State state);

  // True while the amplifier is driving Bunty's reply, for speaking eyes.
  bool speaking() const { return state_ == State::Speaking; }

  // True while a turn is genuinely under way: speech is being heard, or the
  // gateway is working on or delivering a reply. Idle listening is excluded so
  // a quiet room can still let the display sleep normally.
  bool engaged() const;

  // Smoothed loudness of the audio currently reaching the amplifier, 0-100.
  // Drives the mouth so it opens on Bunty's actual speech rather than a timer.
  uint8_t speechLevel() const { return speechLevel_; }
  uint32_t inputLevel() const { return vadLevel_; }
  uint32_t inputThreshold() const { return vadThreshold_; }
  uint32_t inputNoiseFloor() const { return noiseFloorRms_; }
  bool inputSpeech() const { return vadSpeech_; }

 private:
  static void websocketEvent(void *handler, esp_event_base_t base,
                             int32_t eventId, void *eventData);
  static void uploadTask(void *context);
  static void playbackTask(void *context);
  static void networkTask(void *context);
  void serviceNetwork();

  void handleControl(char *payload, size_t length);
  void handleAudio(const uint8_t *data, size_t length);
  void onDisconnected();

  // Both return false if the peer has stopped accepting writes. The caller must
  // then abandon this turn rather than continue queuing stale PCM.
  bool sendFrame(const uint8_t *frame);
  bool sendJson(const char *body);
  void sendPlaybackFinished();

  void pushPreRoll(const uint8_t *frame);
  void flushPreRoll();
  void resetGate();
  void updateNoiseFloor(uint32_t level);
  void requestSocketCleanup();
  void abortInput(const char *reason);
  bool openTurnSocket();

  TapInput *microphone_ = nullptr;
  esp_websocket_client_handle_t client_ = nullptr;
  Speaker speaker_;
  Mp3Stream decoder_;

  StreamBufferHandle_t playback_ = nullptr;
  uint8_t *playbackStorage_ = nullptr;
  StaticStreamBuffer_t playbackControl_ = {};

  // Raised before each task is created and lowered as it retires, so end() can
  // join them before the buffers they read from are freed. A handle would race:
  // a task that fails its own setup can retire before xTaskCreate() returns.
  std::atomic<int8_t> activeTasks_{0};
  std::atomic<bool> running_{false};

  std::atomic<State> state_{State::Disabled};
  std::atomic<bool> socketCleanupRequested_{false};
  std::atomic<bool> closeSessionRequested_{false};
  std::atomic<bool> audioEndSeen_{false};
  std::atomic<bool> acknowledgePending_{false};
  std::atomic<bool> resetGateRequested_{false};
  std::atomic<bool> playbackIdle_{true};
  std::atomic<bool> uploadIdle_{true};
  // A cleanup generation is acknowledged only at each worker's safe point.
  // This prevents a stale "idle" observation racing the start of an I/O call.
  std::atomic<uint32_t> cleanupEpoch_{0};
  std::atomic<uint32_t> uploadEpoch_{0};
  std::atomic<uint32_t> playbackEpoch_{0};
  bool cleanupStarted_ = false;  // voice-net owns this field
  std::atomic<bool> finishPending_{false};
  std::atomic<uint32_t> turn_{0};
  char headers_[384] = {};
  char uri_[256] = {};
  alignas(4) uint8_t uploadFrame_[cfg::voice::kFrameBytes] = {};
  static constexpr size_t kControlBytes = 16384;
  char *controlText_ = nullptr;
  uint8_t *controlArenaStorage_ = nullptr;
  memory::Arena controlArena_;
  ControlMessage controlMessage_;

  std::atomic<uint32_t> connectingSince_{0};
  std::atomic<uint32_t> speakingSince_{0};
  std::atomic<uint32_t> lastSocketActivityAt_{0};

  // Tap-to-talk. A tap starts local capture; speech onset opens cloud turns.
  ConversationWindow conversation_;
  InputSpeechWatchdog inputWatchdog_;

  // WebRTC classifies speech; RMS is only a low-level noise guard/meter.
  // Both engine and gate are owned by voice-up after begin().
  LocalVad vad_;
  SpeechGate gate_;
  uint8_t *preRoll_ = nullptr;
  size_t preRollFrames_ = 0;
  size_t preRollNext_ = 0;
  std::atomic<uint32_t> noiseFloorRms_{0};
  std::atomic<uint32_t> vadLevel_{0};
  std::atomic<uint32_t> vadThreshold_{0};
  std::atomic<bool> vadSpeech_{false};
  std::atomic<uint8_t> speechLevel_{0};
};
