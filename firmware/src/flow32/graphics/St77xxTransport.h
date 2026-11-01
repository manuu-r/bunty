#pragma once
#include "flow32/graphics/DisplayTransport.h"
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
struct St77xxConfig {
  int pinCs=-1, pinDc=-1, pinRst=-1, pinMosi=-1, pinSclk=-1;
  int16_t gramWidth=0, gramHeight=0;
  uint8_t rotation=0;
  uint32_t spiHz=40000000;
};
// Native LCD transport. Each call waits for DMA completion because Flow32
// reuses its bounded internal DMA buffer immediately after present returns.
class St77xxTransport final : public DisplayTransport {
 public:
  explicit St77xxTransport(const St77xxConfig &config):config_(config) {}
  ~St77xxTransport() override { end(); }
  bool begin(const DisplayPanel &panel) override;
  void end() override;
  bool present(const uint16_t *pixels,int16_t x,int16_t y,int16_t width,
               int16_t height,int16_t stride) override;
 private:
  static bool completed(esp_lcd_panel_io_handle_t,esp_lcd_panel_io_event_data_t *,void *);
  St77xxConfig config_;
  esp_lcd_panel_io_handle_t io_=nullptr;
  esp_lcd_panel_handle_t driver_=nullptr;
  StaticSemaphore_t doneStorage_;
  SemaphoreHandle_t done_=nullptr;
  bool busOwned_=false;
  int16_t panelWidth_=0,panelHeight_=0;
};
