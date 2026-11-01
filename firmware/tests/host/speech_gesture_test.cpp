#include "SpeechGesture.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <initializer_list>
int main() {
  using namespace speechGesture;
  Kind kind;
  assert(!parse(nullptr,kind) && !parse("giant",kind));
  for(const char *name:{"neutral","nod","shake","curious","happy"}) {
    assert(parse(name,kind));
    for(uint32_t t=0;t<4000;++t) {
      auto p=pose(kind,t);
      assert(std::isfinite(p.yaw) && std::isfinite(p.pitch));
      assert(std::abs(p.yaw)<=12.001f && std::abs(p.pitch)<=10.001f);
      if(t>=kDurationMs)assert(p.yaw==0 && p.pitch==0);
    }
  }
  request(Kind::Nod,100);request(Kind::Shake,101);
  assert(take(kind,102) && kind==Kind::Shake && !take(kind,103));
  request(Kind::Happy,100);clear();assert(!take(kind,101));
  request(Kind::Curious,100);assert(!take(kind,20101));
  request(Kind::Nod,kClockMask-10);assert(take(kind,kClockMask+20));
  request(Kind::Nod,std::numeric_limits<uint32_t>::max()-10);
  assert(take(kind,20) && kind==Kind::Nod);
  Speed speed;Request reaction;
  assert(!parseSpeed("turbo",speed));
  for(const char *name:{"slow","normal","fast"}) {
    assert(parseSpeed(name,speed));
    for(auto kind:{Kind::Neutral,Kind::Nod,Kind::Shake,Kind::Curious,Kind::Happy}) {
      for(uint32_t t=0;t<=duration(speed)+20;++t) {
        const auto p=pose(kind,t,speed);
        assert(std::isfinite(p.yaw)&&std::isfinite(p.pitch));
        assert(std::abs(p.yaw)<=12.001f&&std::abs(p.pitch)<=10.001f);
        if(t>=duration(speed))assert(p.yaw==0&&p.pitch==0);
      }
    }
  }
  assert(duration(Speed::Fast)==900&&rate(Speed::Fast)==60);
  assert(duration(Speed::Normal)>duration(Speed::Fast));
  assert(duration(Speed::Slow)>duration(Speed::Normal));
  clear();request(Kind::Happy,100,Speed::Fast);onDisplay(150);
  assert(takeVisual(reaction,151)&&reaction.kind==Kind::Happy&&reaction.speed==Speed::Fast);
  assert(!take(kind,152)); // media consumes auto gesture; speech won't repeat it
  request(Kind::Nod,200,Speed::Normal,false);onDisplay(201);
  assert(!takeVisual(reaction,202)&&take(reaction,203)&&reaction.kind==Kind::Nod); // explicit speech stays speech
  requestDisplay(Kind::Curious,100,Speed::Normal);onDisplay(60000);
  assert(takeVisual(reaction,60001)&&reaction.kind==Kind::Curious&&reaction.speed==Speed::Normal);
  requestDisplay(Kind::Shake,100);onDisplay(90101);assert(!takeVisual(reaction,90102));
  requestVisual(Kind::Happy,100);clearSpeech();
  assert(takeVisual(reaction,101)); // listening resumes without cancelling media
  requestVisual(Kind::Nod,100);assert(!takeVisual(reaction,5101));
  const auto generation=cancelGeneration.load();requestVisual(Kind::Nod,100);clearVisual();
  assert(!takeVisual(reaction,101)&&cancelGeneration.load()!=generation);
  requestVisual(Kind::Shake,std::numeric_limits<uint32_t>::max()-10,Speed::Fast);
  assert(takeVisual(reaction,20)&&reaction.kind==Kind::Shake);
  clear();
  puts("speech gestures: bounds, expiry, cancellation and clock wrap passed");
}
