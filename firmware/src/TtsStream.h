#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <strings.h>
#include "Emotion.h"
#include "SpeechGesture.h"
namespace audio {
// Capture relevant headers directly from HTTP events, independent of the
// client's limited saved-header cache. Never guess PCM format from its body.
struct TtsHeaders {
  char type[128]{},order[32]{};
  bool invalid=false;
  template<size_t N> void save(char (&out)[N],const char *value) {
    if(!value || strnlen(value,N)>=N || (out[0] && strcasecmp(out,value))) {invalid=true;return;}
    memcpy(out,value,strlen(value)+1);
  }
  void add(const char *key,const char *value) {
    if(!key)return;
    if(!strcasecmp(key,"Content-Type"))save(type,value);
    if(!strcasecmp(key,"X-Audio-Byte-Order"))save(order,value);
  }
  bool valid() const {
    if(invalid || strcasecmp(order,"little-endian"))return false;
    char copy[sizeof(type)];memcpy(copy,type,sizeof(copy));
    char *p=copy,*end=strchr(p,';');if(end)*end++=0;
    auto trim=[](char *s) {while(*s==' '||*s=='\t')++s;size_t n=strlen(s);while(n && (s[n-1]==' '||s[n-1]=='\t'))s[--n]=0;return s;};
    if(strcasecmp(trim(p),"application/vnd.bunty.speech"))return false;
    bool rate=false,mono=false;
    while(end && *end) {
      p=end;end=strchr(p,';');if(end)*end++=0;
      char *equals=strchr(p,'=');if(!equals)return false;*equals++=0;
      char *key=trim(p),*value=trim(equals);
      const size_t n=strlen(value);
      if(n>=2 && value[0]=='"' && value[n-1]=='"'){value[n-1]=0;++value;}
      if(!strcasecmp(key,"rate")){if(rate || strcmp(value,"16000"))return false;rate=true;}
      if(!strcasecmp(key,"channels")){if(mono || strcmp(value,"1"))return false;mono=true;}
    }
    return rate && mono;
  }
};
enum class TtsFailure {None,Cancelled,Busy,Connect,Request,Headers,Authorization,RateLimit,Service,Format,Power,Speaker,Read,Write,Empty,Truncated,Deadline};
inline const char *ttsError(TtsFailure f) {
  switch(f) {
    case TtsFailure::Busy:return "Speech download busy";
    case TtsFailure::Connect:return "Speech connection failed";
    case TtsFailure::Request:return "Speech request failed";
    case TtsFailure::Headers:return "Speech response timed out";
    case TtsFailure::Authorization:return "Gateway authorization failed";
    case TtsFailure::RateLimit:return "Voice limit reached";
    case TtsFailure::Service:return "Speech service unavailable";
    case TtsFailure::Format:return "Unsupported speech audio";
    case TtsFailure::Power:return "Speaker power busy";
    case TtsFailure::Speaker:return "Speaker initialization failed";
    case TtsFailure::Read:return "Speech stream failed";
    case TtsFailure::Write:return "Speaker playback failed";
    case TtsFailure::Empty:return "Speech response was empty";
    case TtsFailure::Truncated:return "Speech response incomplete";
    case TtsFailure::Deadline:return "Speech playback timed out";
    case TtsFailure::Cancelled:return "Speech cancelled";
    case TtsFailure::None:return "";
  }
  return "Speech failed";
}
struct TtsOutcome {TtsFailure failure=TtsFailure::None;size_t bytes=0;bool ok()const{return failure==TtsFailure::None;}};
// The body of POST /v1/speak: frames of one type byte, a 24-bit little-endian
// length and the payload. A cue applies to the audio that follows it; the end
// frame counts the audio, so a cleanly closed but short stream is still caught.
// A picture frame carries a tiny GIF for the cue that follows it.
enum class FrameType:uint8_t {Audio=1,Cue=2,End=3,Picture=4};
constexpr size_t kPictureMaxBytes=2048;
struct SpeechCue {uint8_t emotion=0;int8_t gesture=-1;};
// "happy,nod" or "happy,". An unknown name degrades to no expression, not an error.
inline SpeechCue parseCue(const char *text) {
  SpeechCue cue;
  char name[16]{};
  const char *comma=strchr(text,',');
  const size_t length=comma?size_t(comma-text):strlen(text);
  if(length<sizeof(name)) {memcpy(name,text,length);cue.emotion=uint8_t(parseEmotion(name));}
  speechGesture::Kind kind;
  if(comma && speechGesture::parse(comma+1,kind))cue.gesture=int8_t(kind);
  return cue;
}
class SpeechDecoder {
 public:
  enum class Result {Ok,Malformed,Stopped};
  // Sink: bool pcm(const int16_t *,size_t), bool cue(const SpeechCue &),
  // uint8_t *pictureBuffer(), void pictureReady(size_t). pcm is caller scratch
  // for 640 samples. Frames may split at any byte. A picture without a free
  // buffer, or too big for one, is skipped: speech matters more.
  template<class Sink> Result feed(const uint8_t *data,size_t size,int16_t *pcm,Sink &sink) {
    while(size) {
      if(finished_)return Result::Malformed;
      if(have_<4) {
        header_[have_++]=*data++;--size;
        if(have_==4) {
          left_=size_t(header_[1])|size_t(header_[2])<<8|size_t(header_[3])<<16;
          used_=0;
          const auto type=FrameType(header_[0]);
          if((type==FrameType::Cue && left_>=sizeof(text_))||(type==FrameType::End && left_!=4))return Result::Malformed;
          picture_=type==FrameType::Picture && left_ && left_<=kPictureMaxBytes?sink.pictureBuffer():nullptr;
        }
      } else if(FrameType(header_[0])==FrameType::Audio) {
        const size_t take=left_<size?left_:size;
        const Result result=audio(data,take,pcm,sink);
        if(result!=Result::Ok)return result;
        data+=take;size-=take;left_-=take;
      } else {
        const size_t take=left_<size?left_:size;
        if(FrameType(header_[0])==FrameType::Cue || FrameType(header_[0])==FrameType::End)memcpy(text_+used_,data,take);
        else if(picture_)memcpy(picture_+used_,data,take);
        used_+=take;data+=take;size-=take;left_-=take;
      }
      if(have_==4 && !left_) {
        have_=0;
        if(FrameType(header_[0])==FrameType::Cue) {
          text_[used_]=0;
          if(!sink.cue(parseCue(text_)))return Result::Stopped;
        } else if(FrameType(header_[0])==FrameType::End) {
          const uint32_t total=uint32_t(uint8_t(text_[0]))|uint32_t(uint8_t(text_[1]))<<8|
            uint32_t(uint8_t(text_[2]))<<16|uint32_t(uint8_t(text_[3]))<<24;
          if(total!=audioBytes_ || carrying_)return Result::Malformed;
          finished_=true;
        } else if(picture_) {
          sink.pictureReady(used_);
        }
      }
    }
    return Result::Ok;
  }
  bool finished() const {return finished_;}
  size_t audioBytes() const {return audioBytes_;}
 private:
  template<class Sink> Result audio(const uint8_t *data,size_t size,int16_t *pcm,Sink &sink) {
    audioBytes_+=size;
    size_t frames=0;
    if(carrying_ && size) {
      pcm[frames++]=int16_t(uint16_t(carry_)|(uint16_t(data[0])<<8));
      carrying_=false;++data;--size;
    }
    for(;size>=2;data+=2,size-=2) {
      pcm[frames++]=int16_t(uint16_t(data[0])|(uint16_t(data[1])<<8));
      if(frames==640) {
        if(!sink.pcm(pcm,frames))return Result::Stopped;
        frames=0;
      }
    }
    if(size) {carry_=data[0];carrying_=true;}
    if(frames && !sink.pcm(pcm,frames))return Result::Stopped;
    return Result::Ok;
  }
  uint8_t header_[4]{},carry_=0;
  uint8_t *picture_=nullptr;
  uint8_t have_=0;
  char text_[32]{};
  size_t left_=0,used_=0,audioBytes_=0;
  bool carrying_=false,finished_=false;
};
// Ops owns the HTTP handle and hands decoded audio to the player. This exact
// pipeline is exercised by host fixtures; buffers are caller-owned PSRAM.
constexpr uint32_t kSpeechLimitMs=90000;
constexpr size_t kSpeechLimitBytes=2880000;  // 90 s: a full 1,023-byte reply
template<class Ops> TtsOutcome streamTts(Ops &ops,const char *body,size_t bodySize,uint8_t *bytes,int16_t *pcm) {
  TtsOutcome result;
  const auto fail=[&](TtsFailure f){result.failure=ops.cancelled()?TtsFailure::Cancelled:f;return result;};
  if(ops.cancelled())return fail(TtsFailure::Cancelled);
  if(!ops.open(bodySize))return fail(TtsFailure::Connect);
  size_t written=0;
  while(written<bodySize){
    if(ops.cancelled())return fail(TtsFailure::Cancelled);
    const int n=ops.send(body+written,bodySize-written);
    if(n<=0 || size_t(n)>bodySize-written)return fail(TtsFailure::Request);
    written+=size_t(n);
  }
  if(!ops.headers())return fail(TtsFailure::Headers);
  if(ops.cancelled())return fail(TtsFailure::Cancelled);
  const int status=ops.status();
  if(status!=200)return fail(status==401 || status==403?TtsFailure::Authorization:
                            status==429?TtsFailure::RateLimit:TtsFailure::Service);
  if(!ops.audioFormat())return fail(TtsFailure::Format);
  ops.streaming();
  SpeechDecoder decoder;
  const uint32_t start=ops.now();
  for(;;){
    if(ops.cancelled())return fail(TtsFailure::Cancelled);
    if(ops.failure()!=TtsFailure::None)return fail(ops.failure());
    if(ops.now()-start>=kSpeechLimitMs)return fail(TtsFailure::Deadline);
    const int n=ops.read(bytes,1280);
    if(ops.cancelled())return fail(TtsFailure::Cancelled);
    if(n<0 || n>1280)return fail(TtsFailure::Read);
    if(n==0){
      if(!ops.complete() || !decoder.finished())return fail(TtsFailure::Truncated);
      break;
    }
    const auto state=decoder.feed(bytes,size_t(n),pcm,ops);
    result.bytes=decoder.audioBytes();
    if(state==SpeechDecoder::Result::Malformed)return fail(TtsFailure::Format);
    if(state==SpeechDecoder::Result::Stopped)return fail(ops.failure());
    if(result.bytes>kSpeechLimitBytes)return fail(TtsFailure::Deadline);
  }
  if(!result.bytes)return fail(TtsFailure::Empty);
  return result;
}
}
