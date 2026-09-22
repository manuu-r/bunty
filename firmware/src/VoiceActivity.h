#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Kept hardware-independent so the exact endpoint/window policy is host-tested.
namespace cfg { namespace voice {
constexpr uint32_t kVadSampleRate = 16000;
constexpr uint32_t kFrameDurationMs = 20;
constexpr size_t kFrameSamples = kVadSampleRate * kFrameDurationMs / 1000;
constexpr size_t kFrameBytes = kFrameSamples * sizeof(int16_t);
// WebRTC modes: 0 least restrictive, 3 most restrictive (NOT VADNet modes).
constexpr int kVadMode = 2;
constexpr uint32_t kMinimumSpeechRms = 100;
constexpr uint32_t kInitialNoiseFloorRms = 70;
constexpr uint32_t kSpeechStartMs = 160;
constexpr uint32_t kNoSpeechTimeoutMs = 4000;
constexpr size_t kPreRollFrames = 300 / kFrameDurationMs;
constexpr uint32_t kConversationWindowMs = 4000;
static_assert(kSpeechStartMs % kFrameDurationMs == 0, "whole VAD frames");
static_assert(kNoSpeechTimeoutMs % kFrameDurationMs == 0, "whole VAD frames");
static_assert(kPreRollFrames * kFrameDurationMs > kSpeechStartMs,
              "pre-roll must cover onset confirmation");
} }

class SpeechGate {
 public:
  void reset() { *this = SpeechGate(); }
  void observe(bool speech) {
    using namespace cfg::voice;
    positiveMs_ = speech ? (positiveMs_ < kSpeechStartMs
                               ? positiveMs_ + kFrameDurationMs : positiveMs_) : 0;
    if (confirmed()) {
      started_ = true;
      quietMs_ = 0;
    } else if (started_ && quietMs_ < kNoSpeechTimeoutMs) {
      quietMs_ += kFrameDurationMs;
    }
  }
  bool started() const { return started_; }
  bool confirmed() const { return positiveMs_ >= cfg::voice::kSpeechStartMs; }
  bool timedOut() const { return started_ && quietMs_ >= cfg::voice::kNoSpeechTimeoutMs; }

 private:
  uint32_t positiveMs_ = 0;
  uint32_t quietMs_ = 0;
  bool started_ = false;
};

// Speech activity is deliberately separate from socket traffic. Both local
// sustained speech and evolving server transcripts must stay alive. Transcript
// progress bounds false-positive VAD/noise; it is NOT a semantic nonsense filter.
class InputSpeechWatchdog {
 public:
  enum class Reason { None, LocalNonSpeech, NoTranscriptProgress };
  void begin(uint32_t now) {
    localAt_ = now;
    transcriptAt_ = now;
    transcriptHash_ = 0;
  }
  void observeLocal(uint32_t now, bool confirmed) {
    if (confirmed) localAt_ = now;
  }
  void observeTranscript(uint32_t now, const char *text) {
    if (!text) return;
    uint32_t hash = 2166136261u;
    bool nonempty = false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
      hash = (hash ^ *p) * 16777619u;
      if (*p > ' ') nonempty = true;
    }
    // Called only by the WebSocket task; repeated updates aren't new evidence.
    if (nonempty && hash != transcriptHash_) {
      transcriptHash_ = hash;
      transcriptAt_ = now;
    }
  }
  Reason reason(uint32_t now) const {
    // A producer can publish a timestamp slightly newer than the caller's
    // sampled 'now'. Signed deltas avoid interpreting that as 49 days idle.
    if (static_cast<int32_t>(now - localAt_.load()) >= int32_t(cfg::voice::kNoSpeechTimeoutMs)) return Reason::LocalNonSpeech;
    if (static_cast<int32_t>(now - transcriptAt_.load()) >= int32_t(cfg::voice::kNoSpeechTimeoutMs)) return Reason::NoTranscriptProgress;
    return Reason::None;
  }

 private:
  std::atomic<uint32_t> localAt_{0};
  std::atomic<uint32_t> transcriptAt_{0};
  uint32_t transcriptHash_ = 0; // WebSocket task only.
};

inline bool gatewayEndsInput(const char *type) {
  return strcmp(type, "input.ended") == 0 || strcmp(type, "input.pause") == 0 ||
         strcmp(type, "assistant.thinking") == 0;
}

// One atomic deadline avoids losing a new touch to a concurrent timeout check.
// Zero means disarmed. All intervals are below INT32_MAX; subtraction wraps.
class ConversationWindow {
 public:
  void arm(uint32_t now) {
    const uint32_t deadline = now + cfg::voice::kConversationWindowMs;
    deadline_ = deadline ? deadline : 1;
  }
  void close() { deadline_ = 0; }
  bool armed() const { return deadline_.load() != 0; }
  bool expire(uint32_t now) {
    uint32_t deadline = deadline_;
    return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0 &&
           deadline_.compare_exchange_strong(deadline, 0);
  }

 private:
  std::atomic<uint32_t> deadline_{0};
};
