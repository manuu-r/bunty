#include "TtsStream.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <initializer_list>
#include <climits>
struct Ops {
  audio::TtsHeaders format;
  std::vector<uint8_t> audio{0x00,0x80,0xff,0x7f,0x01,0x00,0xff,0xff};
  std::vector<int16_t> played;
  size_t offset=0,sendBytes=0;
  int response=200,chunk=1,powerRefusals=0;
  uint32_t clock=0,readDelay=0;
  bool cancel=false,connect=true,sendOk=true,headersOk=true,speaker=true,writeOk=true,done=true,readOk=true;
  bool wasOpened=false,claimed=false,started=false,drained=false,cancelInRead=false;
  Ops(){format.add("Content-Type","audio/l16;rate=16000;channels=1");format.add("X-Audio-Byte-Order","little-endian");}
  bool cancelled(){return cancel;}
  bool open(size_t){return connect;}
  int send(const char *,size_t bytes){if(!sendOk)return -1;const size_t n=std::min<size_t>(2,bytes);sendBytes+=n;return int(n);}
  bool headers(){clock+=8000;return headersOk;} // synthesis can exceed old 4s timeout
  int status(){return response;}
  bool audioFormat(){return format.valid();}
  bool claimAudio(){if(powerRefusals>0){--powerRefusals;return false;}claimed=true;return true;}
  uint32_t now(){return clock;}
  void wait(uint32_t ms){clock+=ms;}
  bool openSpeaker(){wasOpened=true;return speaker;}
  int read(uint8_t *out,size_t cap){
    clock+=readDelay;if(cancelInRead)cancel=true;
    if(!readOk)return -1;
    const size_t n=std::min<size_t>(std::min<size_t>(size_t(chunk),cap),audio.size()-offset);
    std::copy_n(audio.data()+offset,n,out);offset+=n;return int(n);
  }
  bool complete(){return done;}
  bool write(const int16_t *pcm,size_t n){if(!writeOk)return false;played.insert(played.end(),pcm,pcm+n);return true;}
  void level(float f){assert(std::isfinite(f) && f>=0 && f<=1);}
  void playing(){assert(!played.empty());started=true;}
  void drain(){drained=true;}
};
audio::TtsOutcome play(Ops &ops){uint8_t bytes[1281];int16_t pcm[640];return audio::streamTts(ops,"request",7,bytes,pcm);}
int main(){
  using F=audio::TtsFailure;
  for(int chunk:{1,2,3,7,1280}){
    Ops ops;ops.chunk=chunk;ops.powerRefusals=3;
    auto result=play(ops);assert(result.ok() && result.bytes==8 && ops.sendBytes==7 && ops.started && ops.drained);
    assert((ops.played==std::vector<int16_t>{INT16_MIN,INT16_MAX,1,-1}));
  }
  for(int status:{401,403,429,500}){Ops ops;ops.response=status;assert(play(ops).failure==(status==429?F::RateLimit:status==500?F::Service:F::Authorization));assert(!ops.wasOpened && !ops.started);}
  {Ops o;o.connect=false;assert(play(o).failure==F::Connect);}
  {Ops o;o.sendOk=false;assert(play(o).failure==F::Request);}
  {Ops o;o.headersOk=false;assert(play(o).failure==F::Headers);}
  {Ops o;o.format.order[0]=0;assert(play(o).failure==F::Format && !o.wasOpened);}
  {Ops o;o.powerRefusals=1000;assert(play(o).failure==F::Power && !o.wasOpened);}
  {Ops o;o.speaker=false;assert(play(o).failure==F::Speaker && !o.started);}
  {Ops o;o.readOk=false;assert(play(o).failure==F::Read && !o.started);}
  {Ops o;o.writeOk=false;assert(play(o).failure==F::Write && !o.started);}
  {Ops o;o.audio.clear();assert(play(o).failure==F::Empty && !o.started);}
  {Ops o;o.audio.pop_back();assert(play(o).failure==F::Truncated);}
  {Ops o;o.done=false;assert(play(o).failure==F::Truncated);}
  {Ops o;o.cancelInRead=true;assert(play(o).failure==F::Cancelled && !o.started);}
  {Ops o;o.readDelay=60001;assert(play(o).failure==F::Deadline);}
  {Ops o;o.clock=UINT32_MAX-9000;assert(play(o).ok());}
  audio::TtsHeaders headers;
  for(int i=0;i<30;++i)headers.add("Unrelated-Header","ignored");
  headers.add("content-type"," AUDIO/L16 ; channels=\"1\" ; rate = 16000 ");
  headers.add("x-audio-byte-order","little-endian");assert(headers.valid());
  for(const char *type:{"audio/mpeg","audio/l16;rate=24000;channels=1","audio/l16;rate=16000;channels=2","audio/l16;rate=16000;rate=16000;channels=1","audio/l16;rate=16000oops;channels=1","audio/l16;rate=16000"}){
    audio::TtsHeaders h;h.add("Content-Type",type);h.add("X-Audio-Byte-Order","little-endian");assert(!h.valid());
  }
  headers.add("Content-Type","application/json");assert(!headers.valid());
  {audio::TtsHeaders h;char huge[256];std::fill_n(huge,255,'x');huge[255]=0;h.add("Content-Type",huge);assert(h.invalid && !h.valid());}
  puts("TTS: actual playback pipeline, partial sends/PCM chunks, late headers, synthesis wait, failures, power retry, cancellation, drain and wrap passed");
}
