#pragma once
#include <cstdint>
namespace runtime {
// Unsigned subtraction remains correct across millis() wrapping.
class Fade {
 public:
  void target(uint8_t value, uint32_t now) {
    if(value==target_) return;
    start_=sample(now); target_=value; since_=now;
    duration_=target_>start_?224:140;
  }
  uint8_t sample(uint32_t now) const {
    const uint32_t elapsed=now-since_;
    if(elapsed>=duration_ || !duration_) return target_;
    return uint8_t(int(start_)+(int(target_)-int(start_))*int(elapsed)/int(duration_));
  }
 private:
  uint8_t start_=0,target_=0;
  uint32_t since_=0,duration_=0;
};
}
