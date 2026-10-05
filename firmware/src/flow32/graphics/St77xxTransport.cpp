#include "flow32/graphics/St77xxTransport.h"
#include "flow32/graphics/DisplayPanel.h"
#include <esp_lcd_panel_vendor.h>
#include <esp_err.h>
#include "Runtime.h"

bool St77xxTransport::completed(esp_lcd_panel_io_handle_t,esp_lcd_panel_io_event_data_t *,void *context) {
  auto *self=static_cast<St77xxTransport *>(context);
  BaseType_t woken=pdFALSE;
  xSemaphoreGiveFromISR(self->done_,&woken);
  return woken==pdTRUE;
}
bool St77xxTransport::begin(const DisplayPanel &panel) {
  if(driver_) return true;
  if(panel.width<=0 || panel.height<=0 || config_.pinCs<0 || config_.pinDc<0 ||
     config_.pinMosi<0 || config_.pinSclk<0) return false;
  done_=xSemaphoreCreateBinaryStatic(&doneStorage_);
  if(!done_) return false;
  spi_bus_config_t bus={};
  bus.mosi_io_num=config_.pinMosi; bus.miso_io_num=-1; bus.sclk_io_num=config_.pinSclk;
  bus.quadwp_io_num=-1; bus.quadhd_io_num=-1;
  bus.max_transfer_sz=panel.width*panel.height*2;
  if(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO)!=ESP_OK) return false;
  busOwned_=true;
  esp_lcd_panel_io_spi_config_t io={};
  io.dc_gpio_num=gpio_num_t(config_.pinDc); io.cs_gpio_num=gpio_num_t(config_.pinCs);
  io.pclk_hz=config_.spiHz; io.lcd_cmd_bits=8; io.lcd_param_bits=8;
  io.trans_queue_depth=1; io.on_color_trans_done=completed; io.user_ctx=this;
  esp_lcd_panel_dev_config_t device={};
  device.reset_gpio_num=gpio_num_t(config_.pinRst);
  device.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB;
  device.data_endian=LCD_RGB_DATA_ENDIAN_LITTLE;
  device.bits_per_pixel=16;
  if(esp_lcd_new_panel_io_spi(SPI2_HOST,&io,&io_)!=ESP_OK ||
     esp_lcd_new_panel_st7789(io_,&device,&driver_)!=ESP_OK ||
     esp_lcd_panel_reset(driver_)!=ESP_OK || esp_lcd_panel_init(driver_)!=ESP_OK ||
     esp_lcd_panel_invert_color(driver_,true)!=ESP_OK) { end(); return false; }
  // Match Adafruit's rotation 0 (MX|MY), not IDF's unmirrored default.
  const uint8_t rotation=config_.rotation&3;
  if(esp_lcd_panel_swap_xy(driver_,rotation&1)!=ESP_OK ||
     esp_lcd_panel_mirror(driver_,rotation==0 || rotation==3,rotation==0 || rotation==1)!=ESP_OK ||
     esp_lcd_panel_disp_on_off(driver_,true)!=ESP_OK) { end(); return false; }
  panelWidth_=panel.width; panelHeight_=panel.height;
  return true;
}
void St77xxTransport::end() {
  if(driver_) esp_lcd_panel_del(driver_);
  driver_=nullptr;
  if(io_) esp_lcd_panel_io_del(io_);
  io_=nullptr;
  if(busOwned_) spi_bus_free(SPI2_HOST);
  busOwned_=false; panelWidth_=panelHeight_=0;
}
void St77xxTransport::present(const uint16_t *pixels,int16_t x,int16_t y,int16_t width,int16_t height,int16_t stride) {
  if(!driver_ || !pixels || width<=0 || height<=0 || stride<width ||
     x<0 || y<0 || x+width>panelWidth_ || y+height>panelHeight_) return;
  for(int row=0;row<height;++row) {
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(driver_,x,y+row,x+width,y+row+1,pixels+row*stride));
    // Do not return while DMA still references the caller's reusable buffer.
    xSemaphoreTake(done_,portMAX_DELAY);
  }
}
