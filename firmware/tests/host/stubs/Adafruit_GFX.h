#pragma once
#include "Arduino.h"
// Only the base interface used by Display.cpp. The tests exercise the actual
// framebuffer, clipping, dirty tracking and transport code, not a copy of it.
class Adafruit_GFX {
 public:
  Adafruit_GFX(int16_t, int16_t) {}
  virtual ~Adafruit_GFX() = default;
  virtual void drawPixel(int16_t, int16_t, uint16_t) = 0;
  virtual void drawFastHLine(int16_t, int16_t, int16_t, uint16_t) {}
  virtual void drawFastVLine(int16_t, int16_t, int16_t, uint16_t) {}
  virtual void fillRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void setTextColor(uint16_t) {}
  void setTextWrap(bool) {}
};
