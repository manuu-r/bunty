#include "sdkconfig.h"
#if CONFIG_BUNTY_MUSE
#include "Voice.h"
#include "MuseTurnError.h"
#include "Muse.h"
#include "MuseDisplay.h"
#include "MuseVad.h"
#include "SpeechFilter.h"
#include "SpeechGate.h"
#include "BufferedUpload.h"
#include "SpeechGesture.h"
#include <new>
#include "Power.h"
#include "Log.h"
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <atomic>
#include <cmath>
#include <algorithm>

extern "C" {
#include "muse_chat.h"
#include "BuntyMuseHooks.h"
}

namespace {
std::atomic<bool> agentBusy{false};
char pendingReply[1024] = {};
std::atomic<bool> replyReady{false};

struct ReplyReaction {
  Emotion emotion = Emotion::Neutral;
  speechGesture::Kind gesture = speechGesture::Kind::Neutral;
  const char *cleanText = nullptr;
};

inline ReplyReaction parseReaction(const char *text, char *cleanBuf, size_t cleanBufSize) {
  ReplyReaction result;
  if (!text || !text[0]) {
    if (cleanBuf && cleanBufSize) cleanBuf[0] = 0;
    result.cleanText = cleanBuf;
    return result;
  }

  const char *src = text;
  size_t out = 0;

  while (*src) {
    if (*src == '[') {
      const char *close = strchr(src, ']');
      if (close && close - src < 32) {
        char tag[32] = {};
        size_t len = close - src - 1;
        const char *tagStart = src + 1;
        while (*tagStart == ' ' && len > 0) { tagStart++; len--; }
        while (len > 0 && tagStart[len - 1] == ' ') { len--; }
        memcpy(tag, tagStart, len);
        tag[len] = 0;
        for (size_t i = 0; i < len; ++i) {
          if (tag[i] >= 'A' && tag[i] <= 'Z') tag[i] += 32;
        }

        const Emotion em = parseEmotion(tag);
        if (em != Emotion::Neutral || !strcmp(tag, "neutral")) {
          result.emotion = em;
        }
        speechGesture::Kind g;
        if (speechGesture::parse(tag, g)) {
          result.gesture = g;
        }

        src = close + 1;
        if (*src == ' ') src++;
        continue;
      }
    }
    if (cleanBuf && out + 1 < cleanBufSize) {
      cleanBuf[out++] = *src;
    }
    src++;
  }
  if (cleanBuf) {
    while (out > 0 && (cleanBuf[out - 1] == ' ' || cleanBuf[out - 1] == '\n' || cleanBuf[out - 1] == '\r')) out--;
    cleanBuf[out] = 0;
    result.cleanText = cleanBuf;
  }

  // If no explicit gesture was tagged, infer an expressive, cute gesture from the emotion!
  if (result.gesture == speechGesture::Kind::Neutral) {
    switch (result.emotion) {
      case Emotion::Happy:
      case Emotion::Excited:
        result.gesture = speechGesture::Kind::Happy;
        break;
      case Emotion::Curious:
        result.gesture = speechGesture::Kind::PuppyTilt;
        break;
      case Emotion::Sassy:
        result.gesture = speechGesture::Kind::Shimmy;
        break;
      case Emotion::Soft:
        result.gesture = speechGesture::Kind::Bashful;
        break;
      case Emotion::Sad:
      case Emotion::Angry:
        result.gesture = speechGesture::Kind::Shake;
        break;
      default:
        result.gesture = speechGesture::Kind::Nod;
        break;
    }
  }
  return result;
}

} // namespace

extern "C" void bunty_muse_agent_busy(bool busy) {
  agentBusy = busy;
  if (!busy) muse::clearProgress();
}
extern "C" void bunty_muse_agent_activity(const char *activity) {
  if (activity && activity[0]) {
    muse::setProgress(activity, -1);
  } else {
    muse::clearProgress();
  }
}
extern "C" void bunty_muse_queue_tts(const char *text) {
  if (!text || !text[0]) return;
  strlcpy(pendingReply, text, sizeof(pendingReply));
  replyReady = true;
}

bool Voice::begin(Microphone *mic, const char *token) {
  if (ready_) return true;
  microphone_ = mic;
  strlcpy(token_, token, sizeof(token_));

  preRollStorage_ = static_cast<uint8_t *>(memory::external(muse::Vad::kPreRollFrames * sizeof(int16_t)));
  utteranceStorage_ = static_cast<int16_t *>(memory::external(muse::kUtteranceFrames * sizeof(int16_t)));
  gateStorage_ = static_cast<int16_t *>(memory::external(
      size_t(cfg::voice::kGateEdgeMs / 10) * muse::kGateFrameSamples * sizeof(int16_t)));

  auto cleanup = [&] {
    heap_caps_free(gateStorage_); gateStorage_ = nullptr;
    heap_caps_free(preRollStorage_); preRollStorage_ = nullptr;
    heap_caps_free(utteranceStorage_); utteranceStorage_ = nullptr;
  };

  if (!preRollStorage_ || !utteranceStorage_ || !gateStorage_) { cleanup(); return false; }

  if (!bunty_muse_chat_init()) { cleanup(); return false; }

  // Only one worker task for voice capture and turn coordination:
  if (xTaskCreatePinnedToCore(voiceTask, "muse_voice", 8192, this, 4, nullptr, 0) != pdPASS) {
    cleanup(); return false;
  }

  ready_ = true;
  Serial.println("[voice] silent pantomime companion ready; tap or speak to interact");
  return true;
}

bool Voice::gatewayAvailable() const {
  return token_[0] != '\0' && muse::wifiConnected();
}

bool Voice::ready() const {
  return ready_ && (muse_hatch_ready() || gatewayAvailable());
}

bool Voice::working() const { return agentBusy; }

const char *Voice::phaseName(Phase phase) {
  switch (phase) {
    case Phase::Off: return "off";
    case Phase::Connecting: return "connecting";
    case Phase::Listening: return "listening";
    case Phase::Thinking: return "thinking";
    case Phase::Speaking: return "reacting";
    case Phase::Finishing: return "finishing";
  }
  return "unknown";
}

void Voice::setPhase(Phase phase) {
  if (phase_ != phase) {
    logPrintf("[voice] %s -> %s\n", phaseName(phase_), phaseName(phase));
    phase_ = phase;
    phaseSince_ = millis();
  }
}

void Voice::react(const char *replyText) {
  if (!replyText || !replyText[0]) return;
  char cleanText[512] = {};
  const auto reaction = parseReaction(replyText, cleanText, sizeof(cleanText));
  emotion_ = reaction.emotion;
  logPrintf("[voice] react: emotion=%s gesture=%s text=\"%s\"\n",
    emotionName(reaction.emotion), speechGesture::name(reaction.gesture), cleanText);
  speechGesture::request(reaction.gesture, millis(), speechGesture::Speed::Fast);
  muse::setCaption(cleanText);

  // Hold the expressive reaction for a natural duration:
  const size_t len = strlen(cleanText);
  const uint32_t holdMs = std::max<uint32_t>(1800, std::min<uint32_t>(3800, 1400 + uint32_t(len) * 35));
  reactionUntil_ = millis() + holdMs;
  setPhase(Phase::Speaking); // Attention::Speaking renders emotion eyes and cute pose
}

bool Voice::fetchGatewayReply(const int16_t *pcm, size_t frames, char *replyBuf, size_t replyBufSize) {
  if (!token_[0] || !pcm || !frames || !replyBuf || !replyBufSize) return false;
  replyBuf[0] = 0;

  const uint32_t networkAt = millis();
  while (!muse::claimTtsNetwork()) {
    if (toggleRequested_.load() || endRequested_.load()) return false;
    if (millis() - networkAt >= 15000) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  struct NetworkRelease { ~NetworkRelease() { muse::releaseTtsNetwork(); } } release;

  struct ReplyCapture {
    char *buf = nullptr;
    size_t cap = 0;
    bool found = false;
  } capture{replyBuf, replyBufSize};

  const size_t audioBytes = frames * sizeof(int16_t);
  std::string url = std::string("https://") + cfg::voice::kGatewayHost + "/v1/voice/turn";
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 20000;
  config.disable_auto_redirect = true;
  config.user_agent = "Bunty/0.1";
  config.user_data = &capture;
  config.event_handler = [](esp_http_client_event_t *event) -> esp_err_t {
    if (event->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key, "X-Reply")) {
      auto *cap = static_cast<ReplyCapture *>(event->user_data);
      if (cap && cap->buf && cap->cap) {
        // Decode URL percent-encoding (%20 -> space)
        size_t out = 0;
        const char *src = event->header_value;
        while (*src && out + 1 < cap->cap) {
          if (*src == '%' && src[1] && src[2]) {
            auto hex = [](char c) -> int {
              if (c >= '0' && c <= '9') return c - '0';
              if (c >= 'a' && c <= 'f') return c - 'a' + 10;
              if (c >= 'A' && c <= 'F') return c - 'A' + 10;
              return 0;
            };
            cap->buf[out++] = char((hex(src[1]) << 4) | hex(src[2]));
            src += 3;
          } else if (*src == '+') { cap->buf[out++] = ' '; src++; }
          else { cap->buf[out++] = *src++; }
        }
        cap->buf[out] = 0;
        cap->found = out > 0;
      }
    }
    return ESP_OK;
  };

  auto client = esp_http_client_init(&config);
  if (!client) return false;

  std::string auth = std::string("Bearer ") + token_;
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "audio/l16;rate=16000;channels=1");
  esp_http_client_set_header(client, "Accept", "application/json");

  const auto err = esp_http_client_open(client, audioBytes);
  if (err != ESP_OK) {
    esp_http_client_cleanup(client);
    return false;
  }

  // Stream PCM in 4096-byte chunks
  const char *dataPtr = reinterpret_cast<const char *>(pcm);
  size_t sent = 0;
  while (sent < audioBytes) {
    if (toggleRequested_.load() || endRequested_.load()) {
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }
    const size_t chunk = std::min<size_t>(4096, audioBytes - sent);
    const int n = esp_http_client_write(client, dataPtr + sent, chunk);
    if (n <= 0) break;
    sent += size_t(n);
  }

  const bool headersOk = esp_http_client_fetch_headers(client) >= 0;
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  return headersOk && status == 200 && capture.found;
}

void Voice::voiceTask(void *context) { static_cast<Voice *>(context)->run(); }

void Voice::run() {
  int16_t pcm[320];
  muse::Vad vad;
  // Audio is cleaned and classified in 10 ms frames. Before the detector confirms
  // speech the last 320 ms wait in `ring`; from then on a gate keeps only speech
  // and a short margin around it, so silence and background never reach Muse.
  muse::FrameRing ring(reinterpret_cast<int16_t *>(preRollStorage_), muse::Vad::kPreRollFrames / muse::kGateFrameSamples);
  muse::SpeechGate gate(gateStorage_, cfg::voice::kGateEdgeMs / 10);
  SpeechFilter filter;
  filter.begin();
  int16_t slice[muse::kGateFrameSamples], clean[muse::kGateFrameSamples];
  size_t sliceFill = 0;
  muse::UtteranceBuffer utterance(utteranceStorage_);
  bool museDone = false, failed = false, uploading = false, resumeListening = false;
  // The wake-opened turn tolerates long pauses and gives up on silence.
  bool patientNext = false, patientTurn = false;
  size_t listenedFrames = 0;
  constexpr size_t kPatientFrames = size_t(cfg::wake::kSilenceEndMs) * cfg::audio::kSampleRate / 1000;

  auto stop = [&] {
    speechGesture::clear(); detected_ = false; agentBusy = false;
    microphone_->setCapturing(false); microphone_->discard();
    muse_hatch_turn_cancel(); resumeListening = false;
    vad.reset(); ring.reset(); gate.reset(); sliceFill = 0; utterance.reset(); uploading = false; museDone = false;
    reactionUntil_ = 0;
    setPhase(Phase::Off); power::setRadioAwake(false);
  };

  // `resume` keeps a wake turn's patience and elapsed silence: used after noise
  // that turned out not to be speech, so repeated noise cannot hold the mic open.
  auto listen = [&](bool resume = false) {
    speechGesture::clearSpeech(); detected_ = false; lastError_ = ""; agentBusy = false;
    microphone_->setCapturing(false); microphone_->discard();
    vad.reset(); ring.reset(); gate.reset(); sliceFill = 0; utterance.reset(); uploading = false; museDone = false; failed = false;
    reactionUntil_ = 0;
    if (!resume) { patientTurn = patientNext; listenedFrames = 0; }
    patientNext = false;
    vad.setPauseFrames(patientTurn ? kPatientFrames : muse::Vad::kPauseFrames);
    microphone_->setCapturing(true); setPhase(Phase::Listening);
    power::setRadioAwake(true);
  };

  auto finish = [&] {
    detected_ = false;
    microphone_->setCapturing(false);
    // What is still in the microphone ring is only the silence after the last word.
    microphone_->discard();
    const unsigned speechMs = unsigned(gate.speechFrames() * 10);
    if (speechMs < cfg::voice::kMinSpeechMs) {
      // The energy detector fired but there was no voice in it: send nothing.
      logPrintf("[voice] only %u ms of speech in that noise; discarding, still listening\n", speechMs);
      listen(true);
      return;
    }
    setPhase(Phase::Thinking);
    logPrintf("[voice] sending %u ms of speech (%u ms with margins)\n", speechMs,
              unsigned(utterance.frames() * 1000 / cfg::audio::kSampleRate));

    if (muse_hatch_ready()) {
      struct UploadOps {
        Voice &voice;
        uint32_t now() { return millis(); }
        bool cancelled() { return voice.endRequested_ || voice.toggleRequested_; }
        bool connected() { return muse_hatch_ready(); }
        void begin() { muse_hatch_turn_begin(); }
        void audio(const int16_t *data, size_t count) { muse_hatch_turn_audio(data, count); }
        void end() { muse_hatch_turn_end(); }
        void yield() { taskYIELD(); }
        bool failed() {
          char reason[128] = {}; muse_hatch_ev_t event;
          while ((event = muse_hatch_turn_event(reason, sizeof(reason))) != MUSE_HATCH_EV_NONE) {
            if (event == MUSE_HATCH_EV_ERROR) {
              voice.lastError_ = muse::turnError(reason);
              logPrintf("[muse] upload failed: %s\n", voice.lastError_.load()); return true;
            }
          }
          return false;
        }
      } ops{*this};
      const auto outcome = muse::uploadBuffered(utterance, ops);
      if (outcome != muse::UploadResult::Sent) {
        if (outcome == muse::UploadResult::Timeout) lastError_ = "Muse upload timed out";
        else if (outcome == muse::UploadResult::Disconnected) lastError_ = "Muse disconnected during upload";
        else if (outcome == muse::UploadResult::Empty) lastError_ = "Voice note empty";
        stop(); return;
      }
      uploading = false; utterance.reset();
    } else if (gatewayAvailable()) {
      logPrintf("[voice] Muse offline; using Cloudflare Gateway fallback (frames=%u)\n", unsigned(utterance.frames()));
      char replyBuf[512] = {};
      if (fetchGatewayReply(utterance.data(), utterance.frames(), replyBuf, sizeof(replyBuf))) {
        react(replyBuf);
      } else {
        lastError_ = "Gateway turn failed";
        stop(); return;
      }
      uploading = false; utterance.reset();
    } else {
      lastError_ = "Voice connection unavailable";
      stop(); return;
    }
  };

  uint32_t lastSampleAt = 0;
  for (;;) {
    if (endRequested_.exchange(false)) { toggleRequested_ = false; stop(); }
    if (toggleRequested_.exchange(false)) {
      const bool fromWake = wakeTurn_.exchange(false);
      if (phase_ == Phase::Listening) {
        if (uploading) finish(); else stop();
      } else if (phase_ == Phase::Speaking) {
        // Tap interrupts reaction and begins listening immediately
        lastError_ = ""; muse::clearCaption(); listen();
      } else {
        stop();
        if (muse_hatch_ready() || gatewayAvailable()) {
          lastError_ = ""; muse::clearCaption(); resumeListening = true; patientNext = fromWake;
        } else {
          lastError_ = muse::wifiConnected() ? "Muse not connected" : "Wi-Fi not connected";
        }
      }
    }

    if (phase_ == Phase::Listening) {
      if (!muse_hatch_ready() && !gatewayAvailable()) { lastError_ = "Voice disconnected"; stop(); }
      else {
        size_t n = microphone_->read(reinterpret_cast<uint8_t *>(pcm), sizeof(pcm), 20);
        const size_t frames = n / sizeof(int16_t);
        if (frames) {
          const bool servoNoise = int32_t(motionUntil_.load() - millis()) > 0;
          const auto event = vad.process(pcm, frames, servoNoise);
          // Silence is only counted once the head has stopped turning.
          if (!uploading && !servoNoise) listenedFrames += frames;
          if (patientTurn && !uploading && listenedFrames >= kPatientFrames) {
            Serial.println("[voice] nothing said after the wake word; ending");
            lastError_ = ""; stop();
            continue;
          }
          // Clean and classify this chunk in whole 10 ms frames.
          bool overflow = false;
          auto keep = [&](const int16_t *audio, size_t count) { if (!utterance.append(audio, count)) overflow = true; };
          for (size_t at = 0; at < frames;) {
            const size_t take = std::min(frames - at, muse::kGateFrameSamples - sliceFill);
            memcpy(slice + sliceFill, pcm + at, take * sizeof(int16_t));
            sliceFill += take; at += take;
            if (sliceFill < muse::kGateFrameSamples) break;
            sliceFill = 0;
            const bool speech = filter.process(slice, clean);
            if (uploading) gate.push(clean, speech, keep);
            else ring.push(clean, speech);
          }
          if (!uploading) {
            if (event == muse::Vad::Event::Started) {
              muse::clearCaption(); detected_ = true;
              uploading = true;
              gate.reset();
              ring.flush([&](const int16_t *frame, bool speech) { gate.push(frame, speech, keep); });
              if (overflow) { lastError_ = "Voice buffer unavailable"; stop(); }
              else Serial.println("[voice] speech detected; buffering");
            }
          } else if (overflow) {
            lastError_ = "Voice note too long"; stop();
          }
          if (uploading && event == muse::Vad::Event::Ended) {
            Serial.println("[voice] utterance ended; processing reaction"); finish();
          } else if (uploading && event == muse::Vad::Event::Rejected) {
            // A fan or hum that sounded like speech: drop it and keep listening.
            Serial.println("[voice] steady noise, not speech; discarding");
            detected_ = false; uploading = false; utterance.reset(); ring.reset(); gate.reset();
          }
        }
      }
    }

    // Process Muse SDK turn events
    char eventText[128] = {};
    muse_hatch_ev_t event;
    while ((event = muse_hatch_turn_event(eventText, sizeof(eventText))) != MUSE_HATCH_EV_NONE) {
      if (event == MUSE_HATCH_EV_SENT) Serial.println("[muse] utterance acknowledged");
      if (event == MUSE_HATCH_EV_HEARD) Serial.println("[muse] transcription received");
      if (event == MUSE_HATCH_EV_DONE) { Serial.println("[muse] turn complete"); museDone = true; }
      if (event == MUSE_HATCH_EV_ERROR) {
        lastError_ = muse::turnError(eventText);
        logPrintf("[muse] turn failed: %s\n", lastError_.load());
        microphone_->setCapturing(false);
        museDone = true; failed = true;
      }
    }

    // Process Muse reply text when received
    if (replyReady.exchange(false)) {
      react(pendingReply);
      museDone = true;
    }

    if (resumeListening) {
      resumeListening = false;
      if (muse_hatch_ready() || gatewayAvailable()) listen();
      else lastError_ = "Voice disconnected";
    }

    // Reaction duration tracking in Phase::Speaking
    if (phase_ == Phase::Speaking) {
      if (millis() >= reactionUntil_) {
        if (failed || toggleRequested_ || (!muse_hatch_ready() && !gatewayAvailable())) stop();
        else listen(); // Hands-free auto-resume listening!
      }
    }

    if (phase_ != Phase::Off) power::markBusy();
    power::setRadioAwake(phase_ != Phase::Off);
    if (millis() - lastSampleAt >= 5000) { lastSampleAt = millis(); memory::sampleTask("voice"); }
    if (phase_ == Phase::Off) vTaskDelay(pdMS_TO_TICKS(50));
    else if (phase_ != Phase::Listening) vTaskDelay(pdMS_TO_TICKS(10));
  }
}
#endif
