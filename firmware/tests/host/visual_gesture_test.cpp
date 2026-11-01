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
  puts("Visual reactions: actual follower reaches wider held poses at all tempos, rate bounds and return home passed");
}
