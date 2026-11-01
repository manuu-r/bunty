#pragma once
#include <atomic>
#include <cstdint>
namespace audio {
// The reader snapshots a session before blocking in I2S. Stale frames cannot
// enter a new conversation. Only one producer writes the capture ring.
class CaptureGate {
 public:
  uint32_t session() const { return session_.load(); }
  bool current(uint32_t session) const { return (session&1u) && session==session_.load(); }
  void enable(bool enabled) {
    const auto value=session_.load();
    if(bool(value&1u)!=enabled) session_.store(value+1);
  }
  bool beginWrite(uint32_t session) {
    writing_=true;
    if(current(session)) return true;
    writing_=false; return false;
  }
  void endWrite() { writing_=false; }
  bool writing() const { return writing_.load(); }
 private:
  std::atomic<uint32_t> session_{0};
  std::atomic<bool> writing_{false};
};
inline int32_t removeDc(int32_t sample,int32_t &state) {
  // Multiplication is defined for negative samples; left-shifting them is UB.
  state += (sample * 256 - state) >> 9;
  return sample - (state >> 8);
}
}
