#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>

// The most recent "Hey Buddy", kept so Muse can ask where it came from
// (bunty.last_wake). Written by the main task, read by the Muse command task.
namespace wakeInfo {

struct Record {
  uint32_t sequence = 0;       // 0 until the first wake; counts up from there
  uint32_t atMs = 0;           // millis() when it was heard
  bool bearingValid = false;   // the direction estimate was trustworthy
  float bearingRight = 0.0f;   // degrees from where the head pointed; positive is Bunty's right
  float confidence = 0.0f;     // 0..1 correlation between the two microphones
  bool turned = false;         // the head was turned toward the voice
  float lookYaw = 0.0f;        // head offset from home in bunty.look's yaw units
  bool conversationStarted = false;
};

inline std::mutex &lock() {
  static std::mutex mutex;
  return mutex;
}
inline Record &stored() {
  static Record record;
  return record;
}

inline void record(Record heard) {
  std::lock_guard<std::mutex> guard(lock());
  heard.sequence = stored().sequence + 1;
  stored() = heard;
}
inline Record last() {
  std::lock_guard<std::mutex> guard(lock());
  return stored();
}

// Plain words from Bunty's point of view, so Muse need not reason about signs.
inline const char *direction(const Record &r, float aheadWithinDegrees) {
  if (!r.sequence || !r.bearingValid) return "unknown";
  if (std::fabs(r.bearingRight) < aheadWithinDegrees) return "ahead";
  return r.bearingRight > 0.0f ? "right" : "left";
}

}  // namespace wakeInfo
