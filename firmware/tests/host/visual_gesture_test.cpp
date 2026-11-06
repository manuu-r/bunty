#include "SpeechGesture.h"
#include "ServoFollow.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
int main() {
  using namespace speechGesture;
  assert(visualDuration(Speed::Fast)==1300);
  for(auto speed:{Speed::Fast,Speed::Normal,Speed::Slow}) {
    for(auto kind:{Kind::Neutral,Kind::Nod,Kind::Shake,Kind::Curious,Kind::Happy}) {
      float yaw=0,pitch=0,minYaw=0,maxYaw=0,minPitch=0,maxPitch=0;
      for(uint32_t t=0;t<=visualDuration(speed)+400;t+=20) {
        const auto target=visualPose(kind,t,speed);
        assert(std::abs(target.yaw)<=18&&std::abs(target.pitch)<=15);
        const float beforeYaw=yaw,beforePitch=pitch;
        // Exercise the actual motor follower, not just the requested angles.
        yaw=servoFollow::advance(yaw,target.yaw,rate(speed),20).angle;
        pitch=servoFollow::advance(pitch,target.pitch,rate(speed),20).angle;
        assert(std::abs(yaw-beforeYaw)<=rate(speed)*.0201f);
        assert(std::abs(pitch-beforePitch)<=rate(speed)*.0201f);
        minYaw=std::min(minYaw,yaw);maxYaw=std::max(maxYaw,yaw);
        minPitch=std::min(minPitch,pitch);maxPitch=std::max(maxPitch,pitch);
      }
      assert(std::abs(yaw)<.21f&&std::abs(pitch)<.21f); // no residual excursion
      if(kind==Kind::Neutral)assert(minYaw==0&&maxYaw==0&&minPitch==0&&maxPitch==0);
      if(kind==Kind::Nod)assert(maxPitch>14&&minPitch< -13);
      if(kind==Kind::Shake)assert(maxYaw>17&&minYaw< -13);
      if(kind==Kind::Curious)assert(maxYaw>17&&maxPitch>14);
      if(kind==Kind::Happy)assert(maxYaw>15&&minYaw< -13&&maxPitch>13&&minPitch< -12);
    }
  }
  // Holds, cancellation/home targets and a stalled loop remain rate bounded.
  auto p=visualPose(Kind::Curious,500,Speed::Fast);assert(p.yaw==18&&p.pitch==15);
  p=visualPose(Kind::Happy,std::numeric_limits<uint32_t>::max(),Speed::Fast);assert(p.yaw==0&&p.pitch==0);
  const auto step=servoFollow::advance(108,90,60,20);assert(step.angle>=106.79f);
  const auto stalled=servoFollow::advance(90,108,60,1000);assert(stalled.angle<=108);
  // Requested angles replace the swing; timing still comes from the gesture.
  {
    Angles a;a.yaw=10;a.hasYaw=true;a.pitch=-6;a.hasPitch=true;
    Request r{Kind::Nod,Speed::Fast,a};
    float maxYaw=-99,minYaw=99,maxPitch=-99,minPitch=99;
    for(uint32_t t=0;t<=visualDuration(Speed::Fast);t+=10) {
      const auto q=visualPose(r,t),base=visualPose(Kind::Nod,t,Speed::Fast);
      // Same moments of motion as an ordinary nod, scaled to the new peaks.
      assert(std::abs(q.pitch*15+6*base.pitch)<.001f);
      maxYaw=std::max(maxYaw,q.yaw);minYaw=std::min(minYaw,q.yaw);
      maxPitch=std::max(maxPitch,q.pitch);minPitch=std::min(minPitch,q.pitch);
    }
    assert(maxYaw==10&&minYaw==-10); // nod has no yaw of its own: it borrows the nod's timing
    assert(maxPitch==6&&minPitch==-6);
    // Partial angles keep the gesture's default on the other axis.
    Angles onlyYaw;onlyYaw.yaw=5;onlyYaw.hasYaw=true;
    p=visualPose(Request{Kind::Curious,Speed::Fast,onlyYaw},500);assert(p.yaw==5&&p.pitch==15);
    // Neutral with angles is look-hold-return; an unset axis stays home.
    p=visualPose(Request{Kind::Neutral,Speed::Fast,onlyYaw},500);assert(p.yaw==5&&p.pitch==0);
    p=visualPose(Request{Kind::Neutral,Speed::Fast,onlyYaw},1250);assert(p.yaw==0&&p.pitch==0);
    // Without angles the request overloads are the original poses.
    for(uint32_t t=0;t<=2000;t+=50) {
      p=visualPose(Request{Kind::Bashful,Speed::Normal,Angles{}},t);const auto b=visualPose(Kind::Bashful,t,Speed::Normal);
      assert(p.yaw==b.yaw&&p.pitch==b.pitch);
      p=pose(Request{Kind::Happy,Speed::Slow,Angles{}},t);const auto s=pose(Kind::Happy,t,Speed::Slow);
      assert(p.yaw==s.yaw&&p.pitch==s.pitch);
    }
    // Speech: the swing scales to the request and the pose still returns home.
    Angles big;big.yaw=18;big.hasYaw=true;
    for(uint32_t t=0;t<=2000;t+=10)assert(std::abs(pose(Request{Kind::Shake,Speed::Slow,big},t).yaw)<=18.001f);
    p=pose(Request{Kind::Shake,Speed::Slow,big},2000);assert(p.yaw==0&&p.pitch==0);
  }
  puts("Visual reactions: actual follower reaches wider held poses at all tempos, rate bounds and return home passed");
}
