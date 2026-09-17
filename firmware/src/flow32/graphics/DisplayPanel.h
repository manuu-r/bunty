#pragma once

#include <stdint.h>

/**
 * Physical profile for a TFT module.
 * width x height = framebuffer, UI coordinates, and SPI present size (1:1).
 *
 * Concrete board and panel presets live in the app, not here.
 */
struct DisplayPanel {
  const char *id = "display";
  int16_t width = 0;
  int16_t height = 0;
  // The framebuffer is large; PSRAM keeps it clear of the internal heap that
  // Wi-Fi, BLE, and TLS compete for. Falls back to monochrome on failure.
  bool preferPsram = true;
};
