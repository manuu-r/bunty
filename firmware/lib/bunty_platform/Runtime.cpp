#include "Runtime.h"
#include <driver/ledc.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include <nvs_flash.h>
#include <esp_err.h>
#include <esp_pm.h>
namespace {
uint32_t pwmHz[8]={};
int pwmPins[8]={-1,-1,-1,-1,-1,-1,-1,-1};
}
size_t NativeChip::getPsramSize() const { return esp_psram_is_initialized()?esp_psram_get_size():0; }
size_t NativeChip::getFreeHeap() const { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT); }
uint64_t NativeChip::getEfuseMac() const {
  uint8_t mac[6]={}; esp_efuse_mac_get_default(mac);
  uint64_t value=0; memcpy(&value,mac,6); return value;
}
void NativeChip::restart() const { esp_restart(); }
bool ledcSetup(uint8_t channel,uint32_t hz,uint8_t resolution) {
  if(channel>=8) return false;
  // Backlight channel 0 uses timer 0; both servos (2,3) share timer 1.
  ledc_timer_config_t c={}; c.speed_mode=LEDC_LOW_SPEED_MODE;
  c.timer_num=ledc_timer_t(channel/2); c.duty_resolution=ledc_timer_bit_t(resolution);
  c.freq_hz=hz; c.clk_cfg=LEDC_AUTO_CLK;
  if(ledc_timer_config(&c)!=ESP_OK) return false;
  pwmHz[channel]=hz; return true;
}
bool ledcAttachPin(int pin,uint8_t channel) {
  if(channel>=8 || !pwmHz[channel]) return false;
  ledc_channel_config_t c={}; c.gpio_num=pin; c.speed_mode=LEDC_LOW_SPEED_MODE;
  c.channel=ledc_channel_t(channel); c.timer_sel=ledc_timer_t(channel/2);
  if(ledc_channel_config(&c)!=ESP_OK) return false;
  pwmPins[channel]=pin; return true;
}
void ledcWrite(uint8_t channel,uint32_t duty) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE,ledc_channel_t(channel),duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE,ledc_channel_t(channel));
}
void ledcDetachPin(int pin) {
  for(int i=0;i<8;++i) if(pwmPins[i]==pin) {
    ledc_stop(LEDC_LOW_SPEED_MODE,ledc_channel_t(i),0); gpio_reset_pin(gpio_num_t(pin)); pwmPins[i]=-1;
  }
}
void configTime(long,long,const char *a,const char *b,const char *c) {
  if(esp_sntp_enabled()) return;
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0,const_cast<char *>(a));
  esp_sntp_setservername(1,const_cast<char *>(b));
  esp_sntp_setservername(2,const_cast<char *>(c)); esp_sntp_init();
}
void platformInit() {
  // Never erase NVS on an initialization error: it contains the user's pairing.
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  setvbuf(stdout,nullptr,_IONBF,0);
}

bool setCpuFrequencyMhz(uint32_t mhz) {
  esp_pm_config_t config = {};
  config.max_freq_mhz = mhz;
  config.min_freq_mhz = mhz;
  config.light_sleep_enable = false;
  return esp_pm_configure(&config) == ESP_OK;
}
