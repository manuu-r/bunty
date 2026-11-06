#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace speechGesture {
enum class Kind : uint8_t { Neutral, Nod, Shake, Curious, Happy, PuppyTilt, Shimmy, Bashful };
enum class Speed : uint8_t { Slow, Normal, Fast };
// Optional peak angles, whole degrees from home, replacing the gesture's own
// amplitude on that axis. An axis left unset keeps the gesture's default.
struct Angles {
  int8_t yaw=0,pitch=0;bool hasYaw=false,hasPitch=false;
  bool any() const {return hasYaw||hasPitch;}
  uint32_t pack() const {return uint8_t(yaw)|(uint32_t(uint8_t(pitch))<<8)|(hasYaw?1u<<16:0u)|(hasPitch?1u<<17:0u);}
  static Angles unpack(uint32_t word) {
    Angles out;out.yaw=int8_t(word&255);out.pitch=int8_t((word>>8)&255);
    out.hasYaw=(word>>16)&1;out.hasPitch=(word>>17)&1;return out;
  }
};
struct Request {Kind kind=Kind::Neutral;Speed speed=Speed::Fast;Angles angles;};
inline bool parse(const char *name, Kind &out) {
  constexpr const char *names[] = {"neutral", "nod", "shake", "curious", "happy", "puppytilt", "shimmy", "bashful"};
  if(!name) return false;
  for(unsigned i=0;i<8;++i) if(!strcmp(name,names[i])) {out=static_cast<Kind>(i);return true;}
  if(!strcmp(name,"tilt")) {out=Kind::PuppyTilt;return true;}
  if(!strcmp(name,"wiggle")) {out=Kind::Shimmy;return true;}
  if(!strcmp(name,"shy")) {out=Kind::Bashful;return true;}
  return false;
}
inline bool parseSpeed(const char *name,Speed &out) {
  if(!name)return false;
  for(unsigned i=0;i<3;++i)if(!strcmp(name,i==0?"slow":i==1?"normal":"fast")) {out=static_cast<Speed>(i);return true;}
  return false;
}
// Three bounded atomic mailboxes; no SDK callback ever drives a servo.
constexpr uint32_t kClockMask=0x1ffffff;
inline std::atomic<uint32_t> pending{0},displayPending{0},visualPending{0},cancelGeneration{0};
// Angles travel beside each mailbox word. Writers store them first, so a taker
// that sees a request also sees its angles; only two requests racing from
// different tasks in the same instant could pair them wrongly.
inline std::atomic<uint32_t> pendingAngles{0},displayAngles{0},visualAngles{0};
inline uint32_t packed(Kind kind,Speed speed,uint32_t now,bool displayEligible=false) {
  return ((now&kClockMask)<<7)|(displayEligible?64u:0u)|(uint32_t(speed)<<4)|(uint32_t(kind)+1);
}
inline bool takeMailbox(std::atomic<uint32_t> &box,std::atomic<uint32_t> &angles,Request &out,uint32_t now,uint32_t expiry) {
  const uint32_t value=box.exchange(0);
  if(!value||(((now&kClockMask)-(value>>7))&kClockMask)>expiry)return false;
  out={static_cast<Kind>((value&15)-1),static_cast<Speed>((value>>4)&3),Angles::unpack(angles.load())};return true;
}
inline void clearSpeech() {pending.store(0);}
inline void clearVisual() {displayPending.store(0);visualPending.store(0);++cancelGeneration;}
inline void clear() {clearSpeech();clearVisual();}
inline void request(Kind kind,uint32_t now,Speed speed=Speed::Fast,bool displayEligible=true,Angles angles={}) {
  pendingAngles.store(angles.pack());pending.store(packed(kind,speed,now,displayEligible));
}
inline void requestVisual(Kind kind,uint32_t now,Speed speed=Speed::Fast,Angles angles={}) {
  visualAngles.store(angles.pack());visualPending.store(packed(kind,speed,now));
}
inline void requestDisplay(Kind kind,uint32_t now,Speed speed=Speed::Fast,Angles angles={}) {
  displayAngles.store(angles.pack());displayPending.store(packed(kind,speed,now));
}
inline bool visualWaiting() {return visualPending.load()!=0;}
inline bool take(Request &out,uint32_t now) {return takeMailbox(pending,pendingAngles,out,now,20000);}
inline bool take(Kind &kind,uint32_t now) {Request out;if(!take(out,now))return false;kind=out.kind;return true;}
inline bool takeVisual(Request &out,uint32_t now) {return takeMailbox(visualPending,visualAngles,out,now,5000);}
inline void onDisplay(uint32_t now) {
  Request out;
  bool ready=takeMailbox(displayPending,displayAngles,out,now,90000);
  if(!ready) {
    uint32_t value=pending.load();
    const uint32_t angles=pendingAngles.load();
    if((value&64)&&pending.compare_exchange_strong(value,0)&&
       (((now&kClockMask)-(value>>7))&kClockMask)<=20000) {
      out={static_cast<Kind>((value&15)-1),static_cast<Speed>((value>>4)&3),Angles::unpack(angles)};ready=true;
    }
  }
  if(ready) {
    clearSpeech();requestVisual(out.kind,now,out.speed,out.angles);
  }
}
constexpr uint32_t kDurationMs=2000;
inline uint32_t duration(Speed speed) {return speed==Speed::Fast?900:speed==Speed::Normal?1400:kDurationMs;}
inline float rate(Speed speed) {return speed==Speed::Fast?60.f:speed==Speed::Normal?50.f:35.f;}
inline uint32_t visualDuration(Speed speed) {return speed==Speed::Fast?1300:speed==Speed::Normal?1800:2400;}
inline const char *name(Kind kind) {
  constexpr const char *names[]={"neutral","nod","shake","curious","happy","puppytilt","shimmy","bashful"};
  return names[unsigned(kind)<8?unsigned(kind):0];
}
struct Pose {float yaw=0,pitch=0;};
// Hold distinct poses, letting the rate-capped motor reach them. Speech keeps
// the smaller flowing trajectory below; only visual reactions use these arcs.
inline Pose visualPose(Kind kind,uint32_t elapsed,Speed speed=Speed::Fast) {
  const uint32_t total=visualDuration(speed);
  if(elapsed>=total)return {};
  const float t=float(elapsed)/total;
  if(kind==Kind::Curious || kind==Kind::PuppyTilt)return t<.70f?Pose{18,15}:Pose{};
  const float direction=t<.28f?1.f:t<.70f?-1.f:0.f;
  switch(kind) {
    case Kind::Nod:return {0,15*direction};
    case Kind::Shake:return {18*direction,0};
    case Kind::Happy:case Kind::Shimmy:return {16*direction,14*direction};
    case Kind::Bashful:return {-10*direction,-14*direction};
    case Kind::Neutral:case Kind::Curious:case Kind::PuppyTilt:return {};
  }
  return {};
}

inline Pose pose(Kind kind,uint32_t elapsed,Speed speed=Speed::Slow) {
  const uint32_t total=duration(speed);
  if(elapsed>=total)return {};
  const float t=static_cast<float>(elapsed)/total;
  const float envelope=std::sin(3.14159265f*t);
  const float wave=std::sin(6.2831853f*t)*envelope;
  switch(kind) {
    case Kind::Nod:return {0,10*wave};
    case Kind::Shake:return {12*wave,0};
    case Kind::Curious:return {12*envelope,8*envelope};
    case Kind::Happy:return {6*wave,8*wave};
    case Kind::PuppyTilt: {
      const float yawSnap = t < 0.25f ? (t / 0.25f) : (t < 0.75f ? 1.0f : (1.0f - (t - 0.75f) / 0.25f));
      return {12.0f * yawSnap, 8.0f * yawSnap};
    }
    case Kind::Shimmy: {
      const float shimmyWave = std::sin(4.0f * 6.2831853f * t) * envelope;
      return {12.0f * shimmyWave, 4.0f * envelope};
    }
    case Kind::Bashful: {
      return {-8.0f * envelope, -10.0f * envelope};
    }
    case Kind::Neutral:return {};
  }
  return {};
}
// Largest swing each gesture makes, per axis, in degrees.
inline Pose visualPeak(Kind kind) {
  switch(kind) {
    case Kind::Nod:return {0,15};
    case Kind::Shake:return {18,0};
    case Kind::Happy:case Kind::Shimmy:return {16,14};
    case Kind::Bashful:return {-10,-14};
    case Kind::Curious:case Kind::PuppyTilt:return {18,15};
    case Kind::Neutral:return {};
  }
  return {};
}
inline Pose speechPeak(Kind kind) {
  switch(kind) {
    case Kind::Nod:return {0,10};
    case Kind::Shake:return {12,0};
    case Kind::Curious:case Kind::PuppyTilt:return {12,8};
    case Kind::Happy:return {6,8};
    case Kind::Shimmy:return {12,4};
    case Kind::Bashful:return {-8,-10};
    case Kind::Neutral:return {};
  }
  return {};
}
// Keeps the gesture's timing and replaces its swing with the requested angles.
// An axis the gesture does not move borrows the other axis's timing, so
// "nod, yaw 10" turns in step with the nod.
inline Pose withAngles(Pose base,Pose peak,const Angles &angles) {
  const float yawUnit=peak.yaw!=0?base.yaw/peak.yaw:peak.pitch!=0?base.pitch/peak.pitch:0.f;
  const float pitchUnit=peak.pitch!=0?base.pitch/peak.pitch:peak.yaw!=0?base.yaw/peak.yaw:0.f;
  return {(angles.hasYaw?float(angles.yaw):peak.yaw)*yawUnit,
          (angles.hasPitch?float(angles.pitch):peak.pitch)*pitchUnit};
}
// Neutral has no motion of its own: with angles it becomes "look there, hold,
// return", and an axis without an angle stays at home.
inline Kind shapeKind(const Request &request,Angles &angles) {
  angles=request.angles;
  if(request.kind!=Kind::Neutral||!angles.any())return request.kind;
  if(!angles.hasYaw) {angles.hasYaw=true;angles.yaw=0;}
  if(!angles.hasPitch) {angles.hasPitch=true;angles.pitch=0;}
  return Kind::Curious;
}
inline Pose visualPose(const Request &request,uint32_t elapsed) {
  if(!request.angles.any())return visualPose(request.kind,elapsed,request.speed);
  Angles angles;const Kind kind=shapeKind(request,angles);
  return withAngles(visualPose(kind,elapsed,request.speed),visualPeak(kind),angles);
}
inline Pose pose(const Request &request,uint32_t elapsed) {
  if(!request.angles.any())return pose(request.kind,elapsed,request.speed);
  Angles angles;const Kind kind=shapeKind(request,angles);
  return withAngles(pose(kind,elapsed,request.speed),speechPeak(kind),angles);
}
} // namespace speechGesture
