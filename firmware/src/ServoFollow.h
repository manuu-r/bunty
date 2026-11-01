#pragma once
#include <cmath>
namespace servoFollow {
struct Step {float angle;bool moving;};
// Same rate-capped follower used by the physical servo worker, exposed for
// checking whether a reaction gives the motor enough time to reach its poses.
inline Step advance(float current,float target,float rate,float elapsedMs,float timeConstantMs=25.f) {
  const float remaining=target-current;
  if(std::fabs(remaining)<.2f)return {current,false};
  float step=remaining*(1.f-std::exp(-elapsedMs/timeConstantMs));
  const float limit=rate*elapsedMs/1000.f;
  if(step>limit)step=limit;
  if(step< -limit)step= -limit;
  return {current+step,true};
}
}
