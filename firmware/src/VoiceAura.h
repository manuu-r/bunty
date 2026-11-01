#pragma once
#include <cstdint>
namespace voiceAura {
enum class State {Off,Listening,Thinking,Working,Speaking,Connecting,Finishing};
struct Frame {uint16_t color=0;uint8_t opacity=0;};
constexpr uint16_t rgb(int r,int g,int b){return uint16_t((r>>3)<<11|(g>>2)<<5|(b>>3));}
inline Frame blend(Frame a,Frame b,float t) {
  t=t<0?0:t>1?1:t;
  const auto mix=[&](int x,int y){return unsigned(x+(y-x)*t+.5f);};
  return {uint16_t((mix(a.color>>11,b.color>>11)<<11)|
    (mix((a.color>>5)&63,(b.color>>5)&63)<<5)|mix(a.color&31,b.color&31)),
    uint8_t(mix(a.opacity,b.opacity))};
}
inline Frame frame(State state,uint32_t now,unsigned level=0) {
  if(state==State::Off)return {};
  const unsigned at=now%1600;
  const unsigned breath=(at<800?at:1600-at)*3/800;
  const unsigned energy=level>100?100:level;
  switch(state) {
    case State::Listening:return {rgb(193,225,156),uint8_t(5+breath+energy*3/100)};
    case State::Thinking:return {rgb(168,177,245),uint8_t(5+breath)};
    case State::Working:return {rgb(168,177,245),uint8_t(5+(at%800<400?at%800:800-at%800)*5/400)};
    case State::Speaking:return {rgb(233,241,223),uint8_t(5+energy*6/100)};
    case State::Connecting:
    case State::Finishing:return {rgb(129,149,132),5};
    case State::Off:return {};
  }
  return {};
}
}
