#pragma once
#include "Touch.h"
#include <cstdlib>

namespace touch {
// Pure contact state machine, shared by the controller reader and host tests.
class PressTracker {
 public:
  Event sample(int contact, int16_t x, int16_t y, uint32_t now,
               uint32_t holdMs, uint32_t debounceMs, int16_t slop) {
    Event event;
    if (contact < 0) {
      down_ = false;
      blocked_ = true;
      return event;
    }
    if (blocked_) {
      if (contact == 0) { blocked_ = false; releasedAt_ = now; }
      return event;
    }
    if (contact) {
      if (!down_) {
        if (now - releasedAt_ < debounceMs) return event;
        down_ = true;
        held_ = moved_ = false;
        startX_ = x; startY_ = y; downAt_ = now;
      }
      lastX_ = x; lastY_ = y;
      moved_ = moved_ || abs(x - startX_) > slop || abs(y - startY_) > slop;
      if (!held_ && !moved_ && now - downAt_ >= holdMs) {
        held_ = true;
        event.kind = Event::Kind::kLongPress;
        event.x = startX_; event.y = startY_;
      }
    } else if (down_) {
      down_ = false; releasedAt_ = now;
      if (!held_ && !moved_) {
        event.kind = Event::Kind::kTap;
        event.x = lastX_; event.y = lastY_;
      }
    }
    return event;
  }
 private:
  bool down_ = false, held_ = false, moved_ = false, blocked_ = false;
  int16_t startX_ = 0, startY_ = 0, lastX_ = 0, lastY_ = 0;
  uint32_t downAt_ = 0, releasedAt_ = 0;
};
}  // namespace touch
