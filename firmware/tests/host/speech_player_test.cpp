#include "SpeechPictures.h"
#include "SpeechPlayer.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
struct Fired {size_t at;uint8_t emotion;int8_t gesture,picture;};
struct Io {
  std::vector<int16_t> heard;
  std::vector<Fired> cues;
  std::vector<float> levels;
  uint32_t clock=0;
  int refusals=0,opens=0,closes=0,claims=0,releases=0,started=0;
  bool opensOk=true,writesOk=true;
  uint32_t now(){return clock;}
  void wait(uint32_t ms){clock+=ms;}
  bool claim(){if(refusals>0){--refusals;return false;}++claims;return true;}
  void release(){++releases;}
  bool open(){++opens;return opensOk;}
  void close(){++closes;}
  bool write(const int16_t *pcm,size_t frames){if(!writesOk)return false;heard.insert(heard.end(),pcm,pcm+frames);return true;}
  void level(float value){assert(std::isfinite(value)&&value>=0&&value<=1);levels.push_back(value);}
  void playing(){++started;}
  void cue(uint8_t emotion,int8_t gesture,int8_t picture){cues.push_back({heard.size(),emotion,gesture,picture});}
  uint32_t drainMs() const {return 90;}
  float fullScale() const {return 5000.f;}
};
using Player=audio::SpeechPlayer<Io>;
struct Fixture {
  Io io;std::vector<int16_t> ring=std::vector<int16_t>(audio::kPlaybackFrames);Player player{io};
  Fixture(){player.attach(ring.data());}
  void pump(int steps=200){for(int i=0;i<steps;++i)player.service();}
  void feed(size_t frames,int16_t value=1000){
    std::vector<int16_t> pcm(frames,value);assert(player.push(pcm.data(),frames)==frames);
  }
};
int main(){
  using F=audio::TtsFailure;
  {  // Nothing plays until there is a cushion against network stalls.
    Fixture f;f.feed(audio::kPrebufferFrames-1);f.pump();
    assert(f.io.heard.empty() && f.io.opens==0 && !f.player.idle());
    f.feed(1);f.pump();
    assert(f.io.heard.size()==audio::kPrebufferFrames && f.io.opens==1 && f.io.started>0);
  }
  {  // A short reply never reaches the cushion; sealing starts it anyway.
    Fixture f;f.feed(100);f.pump();assert(f.io.heard.empty());
    f.player.seal();f.pump();
    assert(f.io.heard.size()==100 && f.io.closes==1 && f.io.releases==1 && f.player.idle());
    assert(f.io.levels.back()==0);
  }
  {  // Cues fire with the block holding their sample, and in order.
    Fixture f;
    f.player.cue(uint8_t(Emotion::Happy),1);f.feed(audio::kBlockFrames*3);
    f.player.cue(uint8_t(Emotion::Sad),-1);f.feed(audio::kBlockFrames*3);
    f.player.seal();f.pump();
    assert(f.io.cues.size()==2);
    assert(f.io.cues[0].at==0 && f.io.cues[0].gesture==1);
    assert(f.io.cues[1].at==audio::kBlockFrames*3 && f.io.cues[1].emotion==uint8_t(Emotion::Sad));
  }
  {  // A picture travels with its cue, waiting out the audio ahead of it.
    Fixture f;
    f.feed(audio::kBlockFrames*2);f.player.cue(0,-1,2);f.feed(audio::kBlockFrames);
    f.player.seal();f.pump();
    assert(f.io.cues.size()==1 && f.io.cues[0].picture==2 && f.io.cues[0].at==audio::kBlockFrames*2);
  }
  {  // Slots: held until their cue, shown once, reused after release.
    std::vector<uint8_t> storage(audio::SpeechPictures::kSlots*audio::kPictureMaxBytes);
    audio::SpeechPictures pictures;pictures.attach(storage.data());
    const uint8_t *data=nullptr;size_t bytes=0;
    uint8_t *first=pictures.reserve();assert(first);first[0]=42;
    const int8_t a=pictures.commit(5);
    assert(!pictures.take(data,bytes));       // not until its sentence starts
    pictures.fire(a);
    assert(pictures.take(data,bytes) && data==first && data[0]==42 && bytes==5);
    assert(!pictures.take(data,bytes));       // only once
    assert(pictures.reserve()!=first);pictures.commit(1);  // still showing, so kept
    pictures.release();assert(pictures.reserve()==first);pictures.commit(1);
    pictures.clear();
    for(int i=0;i<audio::SpeechPictures::kSlots;++i) {assert(pictures.reserve());pictures.commit(1);}
    assert(!pictures.reserve());              // bounded: the picture is skipped
    pictures.fire(0);pictures.fire(1);        // the newer one wins
    assert(pictures.take(data,bytes) && data==storage.data()+audio::kPictureMaxBytes);
    pictures.release();
    pictures.fire(99);pictures.fire(-1);      // no picture, or nonsense: nothing happens
    assert(!pictures.take(data,bytes));
    pictures.fire(2);pictures.clear();        // a stopped reply leaves nothing behind
    assert(!pictures.take(data,bytes));
    for(int i=0;i<audio::SpeechPictures::kSlots;++i) {assert(pictures.reserve());pictures.commit(1);}
  }
  {  // Level tracks the block about to sound, and stays bounded.
    Fixture f;f.feed(audio::kBlockFrames,0);f.feed(audio::kBlockFrames,32767);f.feed(audio::kBlockFrames,1250);
    f.player.seal();f.pump();
    assert(f.io.levels[0]==0 && f.io.levels[1]==1);
    assert(std::fabs(f.io.levels[2]-.5f)<.01f);
  }
  {  // An underrun holds the amplifier and resumes without replaying a sample.
    Fixture f;f.feed(audio::kPrebufferFrames);f.pump();
    const size_t played=f.io.heard.size();
    assert(f.io.closes==0 && f.io.levels.back()==0);
    f.feed(audio::kRebufferFrames-1);f.pump();assert(f.io.heard.size()==played);
    f.feed(1);f.pump();assert(f.io.heard.size()==played+audio::kRebufferFrames);
    assert(f.io.opens==1 && f.io.closes==0);
  }
  {  // Stop cuts playback and discards the rest, including unfired cues.
    Fixture f;f.feed(audio::kPlaybackFrames);f.player.service();f.player.service();
    f.player.cue(uint8_t(Emotion::Sad),-1);
    f.player.stop();f.pump();
    assert(f.player.idle() && f.io.closes==1 && f.io.releases==1);
    assert(f.io.heard.size()<=audio::kBlockFrames*2 && f.io.cues.empty());
    f.feed(audio::kPrebufferFrames);f.pump();assert(f.io.opens==2 && f.io.cues.empty());
  }
  {  // The ring is bounded; the producer learns how much was taken.
    Fixture f;std::vector<int16_t> pcm(audio::kPlaybackFrames+9);
    assert(f.player.push(pcm.data(),pcm.size())==audio::kPlaybackFrames);
    assert(f.player.push(pcm.data(),1)==0);
  }
  {  // Samples survive ring wrap and arrive unaltered.
    Fixture f;size_t next=0,checked=0;
    for(int round=0;round<40;++round) {
      std::vector<int16_t> pcm(9000);
      for(auto &sample:pcm)sample=int16_t(next++);
      assert(f.player.push(pcm.data(),pcm.size())==pcm.size());
      f.pump(60);
      for(;checked<f.io.heard.size();++checked)assert(f.io.heard[checked]==int16_t(checked));
    }
    assert(next>audio::kPlaybackFrames);
  }
  {  // The amplifier budget may be busy briefly, not forever.
    Fixture f;f.io.refusals=20;f.feed(audio::kPrebufferFrames);f.pump();
    assert(f.player.failure()==F::None && f.io.heard.size()==audio::kPrebufferFrames);
    Fixture g;g.io.refusals=1000;g.feed(audio::kPrebufferFrames);g.pump();
    assert(g.player.failure()==F::Power && g.io.opens==0 && g.player.idle());
    g.player.reset();assert(g.player.failure()==F::None);
  }
  {
    Fixture f;f.io.opensOk=false;f.feed(audio::kPrebufferFrames);f.pump();
    assert(f.player.failure()==F::Speaker && f.io.releases==1 && f.player.idle());
  }
  {
    Fixture f;f.io.writesOk=false;f.feed(audio::kPrebufferFrames);f.pump();
    assert(f.player.failure()==F::Write && f.io.closes==1 && f.io.releases==1 && f.player.idle());
  }
  {  // Sealing an empty stream settles without touching the speaker.
    Fixture f;f.player.seal();assert(!f.player.idle());f.pump();
    assert(f.player.idle() && f.io.opens==0);
  }
  {  // The acknowledgement is audible, bounded, and doesn't count as speech.
    Fixture f;f.player.chime();f.pump();
    assert(f.io.heard.size()==audio::kChimeFrames && f.io.started==0);
    assert(f.io.opens==1 && f.io.closes==1 && f.io.releases==1 && f.player.idle());
    int peak=0;for(int16_t sample:f.io.heard)peak=std::max(peak,std::abs(int(sample)));
    assert(peak>1000 && peak<=1800);
    assert(std::abs(int(f.io.heard.front()))<40 && std::abs(int(f.io.heard.back()))<40);
    assert(std::abs(int(f.io.heard[audio::kChimeFrames/2]))<40);  // no click between notes
  }
  puts("Speech player: prebuffer, sealed short replies, sample-accurate cues, level, underrun, stop, wrap, power and speaker failures, chime passed");
}
