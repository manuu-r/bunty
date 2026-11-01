#include "TtsStream.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <initializer_list>
#include <climits>
using Bytes=std::vector<uint8_t>;
void frame(Bytes &out,audio::FrameType type,const Bytes &payload) {
  out.push_back(uint8_t(type));
  for(int shift=0;shift<24;shift+=8)out.push_back(uint8_t(payload.size()>>shift));
  out.insert(out.end(),payload.begin(),payload.end());
}
void cue(Bytes &out,const std::string &text) {frame(out,audio::FrameType::Cue,Bytes(text.begin(),text.end()));}
void end(Bytes &out,uint32_t bytes) {frame(out,audio::FrameType::End,{uint8_t(bytes),uint8_t(bytes>>8),uint8_t(bytes>>16),uint8_t(bytes>>24)});}
Bytes speech() {
  Bytes out;
  cue(out,"happy,nod");
  frame(out,audio::FrameType::Audio,{0x00,0x80,0xff});  // a sample may span frames
  cue(out,"sad,");
  frame(out,audio::FrameType::Audio,{0x7f,0x01,0x00,0xff,0xff});
  end(out,8);
  return out;
}
struct Ops {
  audio::TtsHeaders format;
  Bytes body=speech();
  std::vector<int16_t> played;
  std::vector<std::pair<size_t,audio::SpeechCue>> cues;
  Bytes picture;uint8_t pictureSlot[audio::kPictureMaxBytes]{};size_t pictureBytes=0;bool pictureFree=true;
  size_t offset=0,sendBytes=0,accept=SIZE_MAX;
  int response=200,chunk=1;
  uint32_t clock=0,readDelay=0;
  audio::TtsFailure player=audio::TtsFailure::None;
  bool cancel=false,connect=true,sendOk=true,headersOk=true,done=true,readOk=true,cancelInRead=false,streamed=false;
  Ops(){format.add("Content-Type","application/vnd.bunty.speech;rate=16000;channels=1");format.add("X-Audio-Byte-Order","little-endian");}
  bool cancelled(){return cancel;}
  bool open(size_t){return connect;}
  int send(const char *,size_t bytes){if(!sendOk)return -1;const size_t n=std::min<size_t>(2,bytes);sendBytes+=n;return int(n);}
  bool headers(){clock+=8000;return headersOk;} // synthesis can exceed old 4s timeout
  int status(){return response;}
  bool audioFormat(){return format.valid();}
  uint32_t now(){return clock;}
  void streaming(){streamed=true;}
  int read(uint8_t *out,size_t cap){
    clock+=readDelay;if(cancelInRead)cancel=true;
    if(!readOk)return -1;
    const size_t n=std::min<size_t>(std::min<size_t>(size_t(chunk),cap),body.size()-offset);
    std::copy_n(body.data()+offset,n,out);offset+=n;return int(n);
  }
  bool complete(){return done;}
  audio::TtsFailure failure(){return player;}
  bool cue(const audio::SpeechCue &next){cues.push_back({played.size(),next});return true;}
  uint8_t *pictureBuffer(){return pictureFree?pictureSlot:nullptr;}
  void pictureReady(size_t bytes){pictureBytes=bytes;picture.assign(pictureSlot,pictureSlot+bytes);}
  bool pcm(const int16_t *samples,size_t frames){
    if(played.size()+frames>accept)return false;
    played.insert(played.end(),samples,samples+frames);return true;
  }
};
audio::TtsOutcome play(Ops &ops){uint8_t bytes[1281];int16_t pcm[640];return audio::streamTts(ops,"request",7,bytes,pcm);}
int main(){
  using F=audio::TtsFailure;
  for(int chunk:{1,2,3,7,1280}){
    Ops ops;ops.chunk=chunk;
    auto result=play(ops);assert(result.ok() && result.bytes==8 && ops.sendBytes==7 && ops.streamed);
    assert((ops.played==std::vector<int16_t>{INT16_MIN,INT16_MAX,1,-1}));
    // The whole point: each cue lands on the sample its sentence starts at.
    assert(ops.cues.size()==2 && ops.cues[0].first==0 && ops.cues[1].first==1);
    assert(ops.cues[0].second.emotion==uint8_t(Emotion::Happy) && ops.cues[0].second.gesture==int8_t(speechGesture::Kind::Nod));
    assert(ops.cues[1].second.emotion==uint8_t(Emotion::Sad) && ops.cues[1].second.gesture==-1);
  }
  for(int status:{401,403,429,500}){Ops ops;ops.response=status;assert(play(ops).failure==(status==429?F::RateLimit:status==500?F::Service:F::Authorization));assert(!ops.streamed);}
  {Ops o;o.connect=false;assert(play(o).failure==F::Connect);}
  {Ops o;o.sendOk=false;assert(play(o).failure==F::Request);}
  {Ops o;o.headersOk=false;assert(play(o).failure==F::Headers);}
  {Ops o;o.format.order[0]=0;assert(play(o).failure==F::Format && !o.streamed);}
  {Ops o;o.readOk=false;assert(play(o).failure==F::Read);}
  {Ops o;o.player=F::Power;assert(play(o).failure==F::Power);}
  {Ops o;o.player=F::Speaker;o.accept=0;assert(play(o).failure==F::Speaker);}
  {Ops o;o.body.clear();assert(play(o).failure==F::Truncated);}  // no end frame at all
  {Ops o;o.body.clear();end(o.body,0);assert(play(o).failure==F::Empty);}
  {Ops o;o.body.pop_back();assert(play(o).failure==F::Truncated);}
  {Ops o;o.done=false;assert(play(o).failure==F::Truncated);}
  {Ops o;o.body.clear();frame(o.body,audio::FrameType::Audio,{1,2});end(o.body,4);assert(play(o).failure==F::Format);} // lost audio
  {Ops o;o.body.clear();frame(o.body,audio::FrameType::Audio,{1,2,3});end(o.body,3);assert(play(o).failure==F::Format);} // half a sample
  {Ops o;o.body.clear();frame(o.body,audio::FrameType::Cue,Bytes(64,'x'));assert(play(o).failure==F::Format);}
  {Ops o;o.body.clear();frame(o.body,audio::FrameType::End,{0});assert(play(o).failure==F::Format);}
  {Ops o;o.body=speech();o.body.push_back(9);assert(play(o).failure==F::Format);}  // data past the end frame
  {Ops o;o.chunk=1280;Bytes tail=speech();o.body.clear();frame(o.body,audio::FrameType(9),{1,2,3});o.body.insert(o.body.end(),tail.begin(),tail.end());assert(play(o).ok() && o.played.size()==4);} // newer frame types are skipped
  {Ops o;o.cancelInRead=true;assert(play(o).failure==F::Cancelled);}
  {Ops o;o.readDelay=audio::kSpeechLimitMs+1;o.chunk=2;assert(play(o).failure==F::Deadline);}
  {Ops o;o.clock=UINT32_MAX-9000;assert(play(o).ok());}
  {Bytes pcm(audio::kSpeechLimitBytes+1280,0);Ops o;o.chunk=1280;o.body.clear();frame(o.body,audio::FrameType::Audio,pcm);assert(play(o).failure==F::Deadline);}
  for(int chunk:{1,3,1280}) {  // a picture may split anywhere and precedes its cue
    Bytes gif={'G','I','F','8','9','a',1,2,3};Ops o;o.chunk=chunk;Bytes tail=speech();o.body.clear();
    frame(o.body,audio::FrameType::Picture,gif);o.body.insert(o.body.end(),tail.begin(),tail.end());
    assert(play(o).ok() && o.picture==gif && o.played.size()==4 && o.cues.size()==2);
  }
  {  // No free slot, or an oversized file, costs the picture and never the speech.
    Bytes gif(audio::kPictureMaxBytes+1,7);Ops o;o.chunk=1280;Bytes tail=speech();o.body.clear();
    frame(o.body,audio::FrameType::Picture,gif);o.body.insert(o.body.end(),tail.begin(),tail.end());
    assert(play(o).ok() && o.picture.empty() && o.played.size()==4);
    Ops busy;busy.pictureFree=false;busy.chunk=1280;busy.body.clear();
    frame(busy.body,audio::FrameType::Picture,{1,2,3});busy.body.insert(busy.body.end(),tail.begin(),tail.end());
    assert(play(busy).ok() && busy.picture.empty() && busy.played.size()==4);
  }
  {auto c=audio::parseCue("excited,shake");assert(c.emotion==uint8_t(Emotion::Excited) && c.gesture==int8_t(speechGesture::Kind::Shake));}
  {auto c=audio::parseCue("grumpy,dance");assert(c.emotion==uint8_t(Emotion::Neutral) && c.gesture==-1);}
  {auto c=audio::parseCue("");assert(c.emotion==uint8_t(Emotion::Neutral) && c.gesture==-1);}
  {auto c=audio::parseCue("a-name-far-longer-than-any-emotion");assert(c.emotion==uint8_t(Emotion::Neutral));}
  audio::TtsHeaders headers;
  for(int i=0;i<30;++i)headers.add("Unrelated-Header","ignored");
  headers.add("content-type"," APPLICATION/VND.BUNTY.SPEECH ; channels=\"1\" ; rate = 16000 ");
  headers.add("x-audio-byte-order","little-endian");assert(headers.valid());
  for(const char *type:{"audio/mpeg","audio/l16;rate=16000;channels=1","application/vnd.bunty.speech;rate=24000;channels=1","application/vnd.bunty.speech;rate=16000;channels=2","application/vnd.bunty.speech;rate=16000;rate=16000;channels=1","application/vnd.bunty.speech;rate=16000oops;channels=1","application/vnd.bunty.speech;rate=16000"}){
    audio::TtsHeaders h;h.add("Content-Type",type);h.add("X-Audio-Byte-Order","little-endian");assert(!h.valid());
  }
  headers.add("Content-Type","application/json");assert(!headers.valid());
  {audio::TtsHeaders h;char huge[256];std::fill_n(huge,255,'x');huge[255]=0;h.add("Content-Type",huge);assert(h.invalid && !h.valid());}
  puts("TTS: framed speech with sample-accurate cues, split frames, late headers, synthesis wait, truncation, malformed frames, player failures, cancellation and wrap passed");
}
