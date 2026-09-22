#pragma once

#include <Arduino.h>
#include <atomic>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>

#include "AudioIo.h"
#include "Config.h"
#include "Memory.h"

// Hands-free conversation with the IoT gateway's /v1/live pipeline
// (Flux STT -> Claude -> Aura TTS).
//
// A tap starts conversation mode. Each turn is its own WebSocket, because the
// gateway closes the socket once a reply has played: Bunty opens it straight
// away, streams the microphone continuously, and lets Flux and Smart Turn
// decide when the user has finished. The reply arrives as raw 16 kHz linear16
// PCM, whatever the `encoding` label on audio.start says. After the speaker
// drains, Bunty acknowledges playback and opens the next turn. Eight seconds
// without anyone talking ends conversation mode.
//
// Tasks: `voice` owns the state machine and the socket's lifetime, the
// IDF WebSocket task only parses and forwards, and `speak` feeds the amplifier.
class Voice {
 public:
  enum class Phase : uint8_t {
    Off,         // Not in conversation mode.
    Connecting,  // Socket opening; the microphone is already buffering.
    Listening,   // Streaming the microphone to Flux.
    Thinking,    // The gateway ended input; Claude and Aura are working.
    Speaking,    // The reply is playing.
    Finishing,   // Playback acknowledged; waiting for the gateway to close.
  };

  bool begin(Microphone *microphone, const char *token, const char *sessionId);
  bool ready() const { return ready_; }

  // The mic control: starts a conversation, ends one that is listening, or
  // interrupts Bunty mid-reply and listens again. Safe from any task.
  void toggle() { toggleRequested_ = true; }

  Phase phase() const { return phase_; }
  bool active() const { return phase_ != Phase::Off; }
  static const char *phaseName(Phase phase);

  // Loudness of what the speaker is playing right now, 0 (silent) to ~1.
  float outputLevel() const { return outputLevel_; }
  uint32_t inputLevel() const { return microphone_ ? microphone_->level() : 0; }

  // A short reason the last conversation ended abnormally, or "" if it didn't.
  const char *lastError() const { return lastError_; }

 private:
  enum class EventKind : uint8_t {
    Opened, Closed, Ready, ReadyUnsupported, Speech, InputEnded,
    AudioStart, AudioEnd, Complete, Error,
  };
  struct Event {
    EventKind kind;
    uint32_t generation;
    char code[32];
  };

  static void voiceTask(void *context);
  static void speakTask(void *context);
  static void socketEvent(void *handler, esp_event_base_t base, int32_t id,
                          void *data);

  void run();
  void step(uint32_t now);
  void handle(const Event &event, uint32_t now);
  void playOnce();

  void start(uint32_t now);
  void endConversation(const char *reason, bool failed);
  void interrupt(uint32_t now);
  void nextTurn(uint32_t now);
  void fail(const char *reason, uint32_t now);

  bool openSocket();
  void closeSocket(bool sayGoodbye);
  bool sendText(const char *text);
  void upload();
  void trackLocalSpeech(uint32_t now);
  bool silenceExpired(uint32_t now) const;
  void setPhase(Phase phase);

  void onText(const char *text, size_t length);
  void onAudio(const uint8_t *data, size_t length);
  void post(EventKind kind, const char *code = "");

  Microphone *microphone_ = nullptr;
  Speaker speaker_;
  esp_websocket_client_handle_t client_ = nullptr;
  QueueHandle_t events_ = nullptr;
  StreamBufferHandle_t playback_ = nullptr;
  StaticStreamBuffer_t playbackControl_ = {};

  std::atomic<bool> ready_{false};
  std::atomic<bool> toggleRequested_{false};
  std::atomic<Phase> phase_{Phase::Off};
  std::atomic<uint32_t> generation_{0};

  // Written by the socket task, consumed by `speak`.
  std::atomic<bool> playing_{false};
  std::atomic<bool> audioEnded_{false};
  std::atomic<bool> drained_{false};
  // Set once a socket is gone; `speak` then empties the ring of its leftovers.
  std::atomic<bool> flush_{false};
  std::atomic<float> outputLevel_{0.0f};

  // Socket-task reassembly of text frames split across data events.
  char *text_ = nullptr;
  size_t textUsed_ = 0;
  uint8_t lastOpcode_ = 0;
  uint8_t *arenaStorage_ = nullptr;
  memory::Arena arena_;
  char turn_[16] = "0";
  char heard_[64] = {};

  // Owned by `voice`.
  bool socketReady_ = false;
  uint8_t failures_ = 0;
  uint32_t phaseSince_ = 0;
  uint32_t retryAt_ = 0;
  uint32_t lastActivityAt_ = 0;
  uint32_t lastLocalSpeechAt_ = 0;
  uint32_t noiseFloor_ = 200;
  uint8_t frame_[cfg::voice::kUploadFrameBytes] = {};
  const char *lastError_ = "";
  char uri_[96] = {};
  char headers_[320] = {};
};
