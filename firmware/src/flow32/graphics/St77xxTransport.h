#pragma once

#include "flow32/graphics/DisplayTransport.h"

#include <Adafruit_ST7789.h>
#include <SPI.h>

/** Board/panel wiring for the ST7789 adapter. */
struct St77xxConfig {
  SPIClass *spi = nullptr;
  int pinCs = -1;
  int pinDc = -1;
  int pinRst = -1;
  int pinMosi = -1;
  int pinSclk = -1;
  int16_t gramWidth = 0;
  int16_t gramHeight = 0;
  uint8_t rotation = 0;
  uint32_t spiHz = 40000000;
};

/**
 * Synchronous Adafruit ST7789 transport.
 *
 * The backlight is deliberately not owned here: Bunty fades it with an LEDC
 * channel, which a digital pin toggle cannot do.
 */
class St77xxTransport final : public DisplayTransport {
public:
  explicit St77xxTransport(const St77xxConfig &config) : config_(config),
      hardware_(config.spi, config.pinCs, config.pinDc, config.pinRst) {}
  ~St77xxTransport() override { end(); }

  bool begin(const DisplayPanel &panel) override;
  void end() override;
  void present(const uint16_t *pixels, int16_t x, int16_t y, int16_t width,
               int16_t height, int16_t stride) override;

private:
  St77xxConfig config_;
  Adafruit_ST7789 hardware_;
  Adafruit_ST7789 *driver_ = nullptr;
  int16_t panelWidth_ = 0;
  int16_t panelHeight_ = 0;
};
