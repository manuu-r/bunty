#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <strings.h>
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
    if(strcasecmp(trim(p),"audio/l16"))return false;
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
enum class TtsFailure {None,Cancelled,Connect,Request,Headers,Authorization,RateLimit,Service,Format,Power,Speaker,Read,Write,Empty,Truncated,Deadline};
inline const char *ttsError(TtsFailure f) {
  switch(f) {
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
// Ops owns HTTP/speaker handles and cleanup. This exact pipeline is exercised
// by host fixtures; buffers are caller-owned PSRAM on the device.
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
  const uint32_t powerAt=ops.now();
  while(!ops.claimAudio()){
    if(ops.cancelled())return fail(TtsFailure::Cancelled);
    if(ops.now()-powerAt>=2000)return fail(TtsFailure::Power);
    ops.wait(10);
  }
  if(!ops.openSpeaker())return fail(TtsFailure::Speaker);
  size_t carry=0;const uint32_t start=ops.now();
  for(;;){
    if(ops.cancelled())return fail(TtsFailure::Cancelled);
    if(ops.now()-start>=60000)return fail(TtsFailure::Deadline);
    const int n=ops.read(bytes+carry,1280);
    if(ops.cancelled())return fail(TtsFailure::Cancelled);
    if(n<0 || n>1280)return fail(TtsFailure::Read);
    if(n==0){
      if(!ops.complete() || carry)return fail(TtsFailure::Truncated);
      if(!result.bytes)return fail(TtsFailure::Empty);
      break;
    }
    const size_t available=carry+size_t(n),frames=available/2;
    uint64_t energy=0;
    for(size_t i=0;i<frames;++i){
      pcm[i]=int16_t(uint16_t(bytes[2*i])|(uint16_t(bytes[2*i+1])<<8));
      energy+=int32_t(pcm[i])*int32_t(pcm[i]);
    }
    if(frames){
      if(!ops.write(pcm,frames))return fail(TtsFailure::Write);
      ops.level(fminf(1,sqrtf(float(energy/frames))/6000));
      ops.playing(); // only after real samples reached I2S
    }
    carry=available%2;if(carry)bytes[0]=bytes[available-1];
    result.bytes+=size_t(n);
    if(result.bytes>1920000)return fail(TtsFailure::Deadline);
  }
  ops.drain();
  if(ops.cancelled())return fail(TtsFailure::Cancelled);
  return result;
}
}
