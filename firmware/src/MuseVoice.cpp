#include "sdkconfig.h"
#if CONFIG_BUNTY_MUSE
#include "Voice.h"
#include "MuseTurnError.h"
#include "Muse.h"
#include "MuseDisplay.h"
#include "MuseVad.h"
#include "BufferedUpload.h"
#include "SpeechGesture.h"
#include "MuseReplyMailbox.h"
#include <new>
#include "Power.h"
#include "Log.h"
#include "TtsRequest.h"
#include "TtsStream.h"
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <cmath>
extern "C" {
#include "muse_chat.h"
#include "BuntyMuseHooks.h"
}
namespace {
muse::ReplyMailbox *replies = nullptr;
struct PlaybackBuffers { uint8_t bytes[1281]; int16_t pcm[640]; char body[audio::kTtsBodyCapacity]; };
TaskHandle_t ttsWorker=nullptr;
PlaybackBuffers *playback = nullptr;
}
extern "C" void bunty_muse_queue_tts(const char *text) {
  if(!replies || !replies->push(text)) Serial.println("[muse] TTS queue full; reply remains in Muse app");
  else xTaskNotifyGive(ttsWorker);
}
bool Voice::begin(Microphone *mic,const char *token) {
  if(ready_) return true;
  microphone_=mic;
  strlcpy(token_,token,sizeof(token_));
  void *mailboxStorage=memory::external(sizeof(muse::ReplyMailbox));
  if(mailboxStorage) replies=new(mailboxStorage) muse::ReplyMailbox;
  playback=static_cast<PlaybackBuffers *>(memory::external(sizeof(PlaybackBuffers)));
  preRollStorage_=static_cast<uint8_t *>(memory::external(muse::Vad::kPreRollFrames*sizeof(int16_t)));
  utteranceStorage_=static_cast<int16_t *>(memory::external(muse::kUtteranceFrames*sizeof(int16_t)));
  auto cleanup = [&] {
    if(replies) { replies->~ReplyMailbox(); heap_caps_free(replies); replies=nullptr; }
    heap_caps_free(playback); playback=nullptr;
    heap_caps_free(preRollStorage_); preRollStorage_=nullptr;
    heap_caps_free(utteranceStorage_);utteranceStorage_=nullptr;
  };
  if(!replies || !playback || !preRollStorage_ || !utteranceStorage_) { cleanup(); return false; }
  // Keep task stacks in internal RAM; only payload storage belongs in PSRAM.
  // TLS gets its own stack instead of nesting under SDK polling/capture.
  if(xTaskCreate(ttsTask,"muse_tts",12288,this,4,&ttsWorker)!=pdPASS) { cleanup(); return false; }
  if(!bunty_muse_chat_init()) {
    vTaskDelete(ttsWorker); ttsWorker=nullptr; cleanup(); return false;
  }
  if(xTaskCreate(voiceTask,"muse_voice",8192,this,4,nullptr)!=pdPASS) {
    vTaskDelete(ttsWorker); ttsWorker=nullptr; cleanup(); return false;
  }
  ready_=true;
  Serial.println("[muse] tap to talk; VAD sends after a pause and listens after replies");
  return true;
}
bool Voice::ready() const { return ready_ && muse_hatch_ready(); }
const char *Voice::phaseName(Phase phase) {
  switch(phase) {
    case Phase::Off:return "off";
    case Phase::Connecting:return "connecting";
    case Phase::Listening:return "listening";
    case Phase::Thinking:return "thinking";
    case Phase::Speaking:return "speaking";
    case Phase::Finishing:return "finishing";
  }
  return "unknown";
}
void Voice::setPhase(Phase phase) {
  if(phase_!=phase) { logPrintf("[muse] %s -> %s\n",phaseName(phase_),phaseName(phase)); phase_=phase; phaseSince_=millis(); }
}
void Voice::voiceTask(void *context) { static_cast<Voice *>(context)->run(); }
void Voice::run() {
  int16_t pcm[320];
  muse::Vad vad;
  muse::PreRoll preRoll(reinterpret_cast<int16_t *>(preRollStorage_));
  muse::UtteranceBuffer utterance(utteranceStorage_);
  bool museDone=false, failed=false, uploading=false, resumeListening=false;
  auto stop = [&] {
    speechGesture::clear();detected_=false;
    microphone_->setCapturing(false); microphone_->discard();
    muse_hatch_turn_cancel(); replies->cancel(); resumeListening=false;
    vad.reset(); preRoll.reset(); utterance.reset(); uploading=false; museDone=false;
    setPhase(replies->empty()?Phase::Off:Phase::Finishing); power::setRadioAwake(!replies->empty());
  };
  auto listen = [&] {
    speechGesture::clearSpeech();detected_=false;lastError_="";
    microphone_->setCapturing(false); microphone_->discard();
    vad.reset(); preRoll.reset(); utterance.reset(); uploading=false; museDone=false; failed=false;
    microphone_->setCapturing(true); setPhase(Phase::Listening);
    power::setRadioAwake(true);
  };
  auto finish = [&] {
    detected_=false;
    microphone_->setCapturing(false);setPhase(Phase::Thinking);
    for(size_t n;(n=microphone_->read(reinterpret_cast<uint8_t *>(pcm),sizeof(pcm),0));)
      if(!utterance.append(pcm,n/sizeof(int16_t))) {lastError_="Voice note too long";stop();return;}
    logPrintf("[muse] buffered note: frames=%u\n",unsigned(utterance.frames()));
    struct UploadOps {
      Voice &voice;
      uint32_t now(){return millis();}
      bool cancelled(){return voice.endRequested_ || voice.toggleRequested_;}
      bool connected(){return muse_hatch_ready();}
      void begin(){muse_hatch_turn_begin();}
      void audio(const int16_t *data,size_t count){muse_hatch_turn_audio(data,count);}
      void end(){muse_hatch_turn_end();}
      void yield(){vTaskDelay(1);}
      bool failed(){
        // Before end() there is no subscription/ACK phase, only SDK errors.
        char reason[128]={};muse_hatch_ev_t event;
        while((event=muse_hatch_turn_event(reason,sizeof(reason)))!=MUSE_HATCH_EV_NONE) {
          if(event==MUSE_HATCH_EV_ERROR) {
            voice.lastError_=muse::turnError(reason);
            logPrintf("[muse] upload failed: %s\n",voice.lastError_.load());return true;
          }
        }
        return false;
      }
    } ops{*this};
    const auto outcome=muse::uploadBuffered(utterance,ops);
    if(outcome!=muse::UploadResult::Sent) {
      if(outcome==muse::UploadResult::Timeout)lastError_="Muse upload timed out";
      else if(outcome==muse::UploadResult::Disconnected)lastError_="Muse disconnected during upload";
      else if(outcome==muse::UploadResult::Empty)lastError_="Voice note empty";
      stop();return;
    }
    uploading=false;utterance.reset();
  };
  uint32_t lastSampleAt=0;
  for(;;) {
    if(playbackFailed_.exchange(false)) {
      // Local speech failure does not reject an accepted Muse/tool request.
      logPrintf("[muse] speech unavailable: %s; turn continues\n",lastError_.load());
    }
    if(endRequested_.exchange(false)) {toggleRequested_=false;stop();}
    if(toggleRequested_.exchange(false)) {
      if(phase_==Phase::Listening) {
        if(uploading) finish(); else stop();
      } else {
        stop();
        if(muse_hatch_ready()) {
          lastError_=""; muse::clearCaption(); resumeListening=true;
        } else { lastError_="Muse not connected"; }
      }
    }
    if(phase_==Phase::Listening) {
      if(!muse_hatch_ready()) { lastError_="Muse disconnected"; stop(); }
      else {
        size_t n=microphone_->read(reinterpret_cast<uint8_t *>(pcm),sizeof(pcm),20);
        const size_t frames=n/sizeof(int16_t);
        if(frames) {
          const auto event=vad.process(pcm,frames);
          if(!uploading) {
            preRoll.push(pcm,frames);
            if(event==muse::Vad::Event::Started) {
              muse::clearCaption();detected_=true;
              uploading=true;
              bool captured=true;
              preRoll.flush([&](const int16_t *audio,size_t count) {captured=utterance.append(audio,count) && captured;});
              if(!captured) {lastError_="Voice note buffer unavailable";stop();}
              else Serial.println("[muse] speech detected; buffering");
            }
          } else if(!utterance.append(pcm,frames)) {lastError_="Voice note too long";stop();}
          if(uploading && event==muse::Vad::Event::Ended) {
            Serial.println("[muse] utterance ended; sending"); finish();
          }
        }
        // Silence opens no request. Stay ready across screen updates until
        // explicitly ended; connection and playback failures still stop safely.
      }
    }
    char eventText[128]={};
    muse_hatch_ev_t event;
    while((event=muse_hatch_turn_event(eventText,sizeof(eventText)))!=MUSE_HATCH_EV_NONE) {
      if(event==MUSE_HATCH_EV_SENT) Serial.println("[muse] utterance acknowledged");
      if(event==MUSE_HATCH_EV_HEARD) Serial.println("[muse] transcription received");
      if(event==MUSE_HATCH_EV_DONE) {Serial.println("[muse] turn complete");museDone=true;}
      if(event==MUSE_HATCH_EV_ERROR) {
        // Do not print SDK error text: it may include remote diagnostics.
        lastError_=muse::turnError(eventText);
        logPrintf("[muse] turn failed: %s\n",lastError_.load());
        microphone_->setCapturing(false); replies->cancel();
        museDone=true; failed=true;
      }
    }
    // Only this task owns conversation phase and the SDK. The worker owns
    // HTTP/I2S and releases its slot only after the speaker is silent.
    if(phase_==Phase::Finishing && replies->empty()) setPhase(Phase::Off);
    if(resumeListening && replies->empty()) {
      resumeListening=false;
      if(muse_hatch_ready()) listen();
      else lastError_="Muse disconnected";
    }
    if(phase_==Phase::Thinking && speaking_) setPhase(Phase::Speaking);
    if(phase_==Phase::Speaking && !speaking_) setPhase(Phase::Thinking);
    if(museDone && replies->empty()) {
      if(failed || toggleRequested_ || !muse_hatch_ready()) stop();
      else listen();
    }
    if(phase_!=Phase::Off) power::markBusy();
    power::setRadioAwake(phase_!=Phase::Off);
    if(millis()-lastSampleAt>=5000) { lastSampleAt=millis(); memory::sampleTask("muse-voice"); }
    vTaskDelay(pdMS_TO_TICKS(phase_==Phase::Off?50:10));
  }
}
void Voice::ttsTask(void *context) {
  auto *voice=static_cast<Voice *>(context);
  for(;;) {
    if(const auto *reply=replies->peek()) {
      if(!replies->cancelled(reply->generation) && !voice->playTts(reply->text,reply->generation)
         && !replies->cancelled(reply->generation) && !voice->toggleRequested_ && !voice->endRequested_)
        voice->playbackFailed_=true;
      voice->outputLevel_=0;
      voice->speaking_=false;
      memory::sampleTask("muse-tts");
      replies->release();
    } else ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
  }
}
bool Voice::playTts(const char *text,uint32_t generation) {
  const auto cancelled=[&] {return replies->cancelled(generation) || toggleRequested_.load() || endRequested_.load();};
  if(!token_[0]) {lastError_="Gateway token missing";return false;}
  const uint32_t networkAt=millis();
  while(!muse::claimTtsNetwork()) {
    if(cancelled())return false;
    if(millis()-networkAt>=15000) {lastError_="Speech download busy";return false;}
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  struct NetworkRelease {~NetworkRelease(){muse::releaseTtsNetwork();}} release;
  const size_t bodySize=audio::ttsRequest(text,playback->body,sizeof(playback->body));
  if(!bodySize) {lastError_="TTS request too large";return false;}
  audio::TtsHeaders headers;
  std::string url=std::string("https://")+cfg::voice::kGatewayHost+"/v1/tts";
  esp_http_client_config_t config={};
  config.url=url.c_str();config.method=HTTP_METHOD_POST;
  config.crt_bundle_attach=esp_crt_bundle_attach;
  config.timeout_ms=15000;config.disable_auto_redirect=true;
  config.user_agent="Bunty/0.1";config.user_data=&headers;
  config.event_handler=[](esp_http_client_event_t *event)->esp_err_t {
    if(event->event_id==HTTP_EVENT_ON_HEADER)
      static_cast<audio::TtsHeaders *>(event->user_data)->add(event->header_key,event->header_value);
    return ESP_OK;
  };
  auto client=esp_http_client_init(&config);
  if(!client) {lastError_="TTS allocation failed";return false;}
  std::string auth=std::string("Bearer ")+token_;
  esp_http_client_set_header(client,"Authorization",auth.c_str());
  esp_http_client_set_header(client,"Content-Type","application/json");
  struct Ops {
    Voice &voice;esp_http_client_handle_t client;audio::TtsHeaders &metadata;uint32_t generation;
    bool opened=false,claimed=false,started=false;
    bool cancelled(){return replies->cancelled(generation)||voice.toggleRequested_||voice.endRequested_;}
    uint32_t now(){return millis();}
    void wait(uint32_t ms){vTaskDelay(pdMS_TO_TICKS(ms));}
    bool open(size_t bytes){
      logPrintf("[muse-tts] connecting: internal_free=%u stack_free=%u\n",
        unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),unsigned(uxTaskGetStackHighWaterMark(nullptr)));
      const auto error=esp_http_client_open(client,bytes);
      opened=error==ESP_OK;
      if(!opened)logPrintf("[muse-tts] connect failed: %s\n",esp_err_to_name(error));
      return opened;
    }
    int send(const char *data,size_t size){return esp_http_client_write(client,data,size);}
    bool headers(){return esp_http_client_fetch_headers(client)>=0;}
    int status(){const int s=esp_http_client_get_status_code(client);logPrintf("[muse-tts] HTTP status=%d\n",s);return s;}
    bool audioFormat(){const bool valid=metadata.valid();if(!valid)Serial.println("[muse-tts] invalid/missing PCM headers");return valid;}
    bool claimAudio(){claimed=power::claim(power::Load::kAudio);return claimed;}
    bool openSpeaker(){esp_http_client_set_timeout_ms(client,3000);return voice.speaker_.open();}
    int read(uint8_t *data,size_t size){return esp_http_client_read(client,reinterpret_cast<char *>(data),size);}
    bool complete(){return esp_http_client_is_complete_data_received(client);}
    bool write(const int16_t *pcm,size_t frames){return voice.speaker_.write(pcm,frames,cfg::audio::kVolumePercent);}
    void level(float value){voice.outputLevel_=value;}
    void playing(){if(!started){started=true;Serial.println("[muse-tts] playback started");}voice.speaking_=true;}
    void drain(){const uint32_t at=millis();while(!cancelled() && millis()-at<cfg::audio::kSpeakerDrainMs)wait(10);}
    ~Ops(){
      voice.speaker_.close();voice.outputLevel_=0;voice.speaking_=false;
      if(claimed)power::release(power::Load::kAudio);
      if(opened)esp_http_client_close(client);
      esp_http_client_cleanup(client);
    }
  } ops{*this,client,headers,generation};
  const auto outcome=audio::streamTts(ops,playback->body,bodySize,playback->bytes,playback->pcm);
  logPrintf("[muse-tts] finished: code=%u bytes=%u stack_free=%u\n",unsigned(outcome.failure),unsigned(outcome.bytes),unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  if(!outcome.ok() && outcome.failure!=audio::TtsFailure::Cancelled)lastError_=audio::ttsError(outcome.failure);
  return outcome.ok();
}
#endif
