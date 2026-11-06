#pragma once
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "TtsStream.h"
namespace audio {
// A power of two keeps index wrapping correct across uint32_t overflow.
constexpr size_t kPlaybackFrames=65536;  // 4 s of PSRAM
constexpr size_t kPrebufferFrames=2400;  // 150 ms rides out Wi-Fi and TLS stalls
constexpr size_t kRebufferFrames=1200;   // 75 ms, after an underrun
constexpr size_t kBlockFrames=320;       // 20 ms: the resolution of cues and level
constexpr size_t kChimeFrames=3200;      // 200 ms
constexpr uint32_t kAudioClaimMs=2000;
constexpr uint16_t kCues=16;

struct Cue {uint32_t at;uint8_t emotion;int8_t gesture,picture;};

// Soft rising two-note acknowledgement, windowed so it neither clicks nor pops.
inline int16_t chimeSample(size_t frame) {
  const float t=float(frame)/16000.f;
  const bool second=frame>=kChimeFrames/2;
  const float note=second?t-.1f:t;
  const float window=std::sin(3.14159265f*note/.1f);
  return int16_t(1800.f*window*window*std::sin(6.2831853f*(second?880.f:660.f)*note));
}

// Sample position is the one clock: cues fire and the level is published as
// samples enter the 60 ms I2S queue, which leads the sound just enough to cover
// the display's frame time and the servos' lag.
//
// One producer (the speech fetch) and one consumer (service(), on the audio
// task). The ring keeps a slow network and the speaker independent, so TLS
// stalls neither gap the audio nor drift the mouth away from it.
template<class Io> class SpeechPlayer {
 public:
  explicit SpeechPlayer(Io &io):io_(io){}
  void attach(int16_t *storage) {storage_=storage;}

  // Producer. Returns the frames accepted, possibly fewer than offered.
  size_t push(const int16_t *pcm,size_t frames) {
    const uint32_t head=head_.load();
    const size_t room=kPlaybackFrames-(head-tail_.load());
    const size_t count=frames<room?frames:room;
    for(size_t i=0;i<count;++i)storage_[(head+i)%kPlaybackFrames]=pcm[i];
    head_.store(head+uint32_t(count));
    return count;
  }
  // Applies from the next sample pushed. A full list drops the expression, not speech.
  bool cue(uint8_t emotion,int8_t gesture,int8_t picture=-1) {
    const uint16_t head=cueHead_.load();
    if(uint16_t(head-cueTail_.load())>=kCues)return false;
    cues_[head%kCues]={head_.load(),emotion,gesture,picture};
    cueHead_.store(uint16_t(head+1));
    return true;
  }
  // Everything pushed so far is the whole stream: play it out without waiting for more.
  void seal() {sealed_.store(true);}
  void chime() {chime_.store(true);}
  // Any task. Silences within one block and discards what remains.
  void stop() {stop_.store(true);}
  void reset() {failure_.store(TtsFailure::None);}
  TtsFailure failure() const {return failure_.load();}
  bool idle() const {
    return state_.load()==State::Idle && !sealed_.load() && !stop_.load() && head_.load()==tail_.load();
  }

  // Consumer: one bounded step of the audio task.
  void service() {
    if(stop_.exchange(false)) {discard();finish();return;}
    switch(state_.load()) {
      case State::Idle:
        if(chime_.exchange(false))playChime();
        else if(available()>=kPrebufferFrames || (sealed_.load() && available()))begin();
        else {
          // Pushes precede seal(), so sealed and empty means nothing is coming.
          if(sealed_.load() && !available())sealed_.store(false);
          io_.wait(5);
        }
        return;
      case State::Waiting:
        if(available()>=kRebufferFrames || sealed_.load())state_.store(State::Playing);
        else io_.wait(5);
        return;
      case State::Playing:
        play();
        return;
    }
  }
 private:
  enum class State:uint8_t {Idle,Waiting,Playing};
  size_t available() const {return head_.load()-tail_.load();}
  void discard() {tail_.store(head_.load());cueTail_.store(cueHead_.load());}
  bool acquire() {
    const uint32_t started=io_.now();
    while(!io_.claim()) {
      if(stop_.load() || io_.now()-started>=kAudioClaimMs)return false;
      io_.wait(10);
    }
    return true;
  }
  void begin() {
    if(!acquire()) {discard();sealed_.store(false);failure_.store(TtsFailure::Power);return;}
    if(!io_.open()) {io_.release();discard();sealed_.store(false);failure_.store(TtsFailure::Speaker);return;}
    state_.store(State::Playing);
  }
  void finish() {
    if(state_.load()!=State::Idle) {io_.level(0);io_.close();io_.release();}
    cueTail_.store(cueHead_.load());sealed_.store(false);state_.store(State::Idle);
  }
  void play() {
    const uint32_t tail=tail_.load();
    const size_t left=head_.load()-tail,count=left<kBlockFrames?left:kBlockFrames;
    if(!count) {
      if(sealed_.load()) {
        const uint32_t at=io_.now();
        while(!stop_.load() && io_.now()-at<io_.drainMs())io_.wait(10);
        finish();
      } else {
        // Keep the amplifier on through an underrun; I2S plays silence meanwhile.
        io_.level(0);state_.store(State::Waiting);
      }
      return;
    }
    uint64_t energy=0;
    for(size_t i=0;i<count;++i) {
      block_[i]=storage_[(tail+i)%kPlaybackFrames];
      energy+=int32_t(block_[i])*int32_t(block_[i]);
    }
    for(uint16_t at=cueTail_.load();at!=cueHead_.load();at=cueTail_.load()) {
      const Cue &next=cues_[at%kCues];
      if(int32_t(next.at-(tail+uint32_t(count)))>=0)break;
      io_.cue(next.emotion,next.gesture,next.picture);
      cueTail_.store(uint16_t(at+1));
    }
    if(!io_.write(block_,count)) {
      discard();failure_.store(TtsFailure::Write);finish();return;
    }
    tail_.store(tail+uint32_t(count));
    // The square root opens the mouth on quiet syllables too.
    const float level=std::sqrt(std::sqrt(float(energy/count))/io_.fullScale());
    io_.level(level<1?level:1);
    io_.playing();
  }
  void playChime() {
    if(!acquire())return;
    if(!io_.open()) {io_.release();return;}
    for(size_t done=0;done<kChimeFrames && !stop_.load();done+=kBlockFrames) {
      for(size_t i=0;i<kBlockFrames;++i)block_[i]=chimeSample(done+i);
      if(!io_.write(block_,kBlockFrames))break;
    }
    io_.close();io_.release();
  }
  Io &io_;
  int16_t *storage_=nullptr;
  int16_t block_[kBlockFrames];
  Cue cues_[kCues]{};
  std::atomic<uint32_t> head_{0},tail_{0};
  std::atomic<uint16_t> cueHead_{0},cueTail_{0};
  std::atomic<State> state_{State::Idle};
  std::atomic<bool> sealed_{false},stop_{false},chime_{false};
  std::atomic<TtsFailure> failure_{TtsFailure::None};
};
}
