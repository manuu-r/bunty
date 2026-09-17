#include "flow32/graphics/St77xxTransport.h"

#include "flow32/graphics/DisplayPanel.h"

#include <Arduino.h>

bool St77xxTransport::begin(const DisplayPanel &panel) {
  if (driver_) return true;
  if (panel.width <= 0 || panel.height <= 0 || !config_.spi ||
      config_.pinCs < 0 || config_.pinDc < 0 || config_.pinMosi < 0 ||
      config_.pinSclk < 0 || config_.gramWidth <= 0 ||
      config_.gramHeight <= 0) {
    return false;
  }

  config_.spi->begin(config_.pinSclk, -1, config_.pinMosi, -1);
  driver_ = &hardware_;
  driver_->init(config_.gramWidth, config_.gramHeight);
  driver_->setSPISpeed(config_.spiHz);
  driver_->setRotation(config_.rotation);
  panelWidth_ = panel.width;
  panelHeight_ = panel.height;
  return true;
}

void St77xxTransport::end() {
  driver_ = nullptr;
  panelWidth_ = 0;
  panelHeight_ = 0;
}

void St77xxTransport::present(const uint16_t *pixels, int16_t x, int16_t y,
                              int16_t width, int16_t height, int16_t stride) {
  if (!driver_ || !pixels || width <= 0 || height <= 0 || stride < width ||
      x < 0 || y < 0 || x + width > panelWidth_ || y + height > panelHeight_) {
    return;
  }
  driver_->startWrite();
  driver_->setAddrWindow(x, y, static_cast<uint16_t>(width),
                         static_cast<uint16_t>(height));
  if (stride == width) {
    driver_->writePixels(const_cast<uint16_t *>(pixels),
                         static_cast<uint32_t>(width) * height);
  } else {
    for (int16_t row = 0; row < height; ++row) {
      driver_->writePixels(
          const_cast<uint16_t *>(pixels + static_cast<int32_t>(row) * stride),
          static_cast<uint32_t>(width));
    }
  }
  driver_->endWrite();
}
