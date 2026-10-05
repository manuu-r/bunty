#pragma once

#include "Runtime.h"

namespace touch {

struct Event {
  enum class Kind : uint8_t { kNone, kTap, kLongPress };
  Kind kind = Kind::kNone;
  int16_t x = 0;
  int16_t y = 0;
};

bool begin();
bool available();

// The next queued event, or kNone. A reader task samples the panel
// independently of loop(), so a slow frame or a blocking screen cannot miss a
// press, and it is safe to call this from any task.
Event poll();

// Drops anything already queued, so a state change does not immediately act on
// the touch that caused it.
void flush();

}  // namespace touch
