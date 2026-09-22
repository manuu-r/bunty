#pragma once

#include <Arduino.h>

namespace hearing {

struct Bearing {
  // Negative is to Bunty's left, positive to its right.
  float degrees = 0.0f;
  uint8_t confidence = 0;
  uint32_t at = 0;
};

void begin();

// Raw interleaved I2S slots, exactly as i2s_read() delivered them.
void observe(const int32_t *slots, size_t count, uint32_t now);

// A source behind Bunty mirrors onto the same bearing as one in front.
bool bearing(Bearing *out);

void reset();

}  // namespace hearing
