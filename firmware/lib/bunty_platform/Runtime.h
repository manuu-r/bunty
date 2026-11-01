#pragma once
// Small native IDF platform boundary; no Arduino core is linked.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_timer.h>
using String = std::string;
using std::min;
using std::max;
using std::isfinite;
constexpr float PI = 3.14159265358979323846f;
template<class T, class U, class V> inline T constrain(T x,U lo,V hi) {
  return x < lo ? T(lo) : x > hi ? T(hi) : x;
}
#define PROGMEM
#define pgm_read_byte(p) (*reinterpret_cast<const uint8_t *>(p))
#define pgm_read_word(p) (*reinterpret_cast<const uint16_t *>(p))
class __FlashStringHelper;
constexpr int INPUT=0, OUTPUT=1, INPUT_PULLUP=2, LOW=0, HIGH=1;
inline uint32_t millis() { return uint32_t(esp_timer_get_time()/1000); }
inline void delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }
inline float radians(float degrees) { return degrees * 0.017453292519943295f; }
inline void yield() { taskYIELD(); }
inline void pinMode(int pin,int mode) {
  gpio_config_t c={}; c.pin_bit_mask=1ULL<<pin;
  c.mode=mode==OUTPUT?GPIO_MODE_OUTPUT:GPIO_MODE_INPUT;
  c.pull_up_en=mode==INPUT_PULLUP?GPIO_PULLUP_ENABLE:GPIO_PULLUP_DISABLE;
  gpio_config(&c);
}
inline void digitalWrite(int pin,int value) { gpio_set_level(gpio_num_t(pin),value); }
inline int digitalRead(int pin) { return gpio_get_level(gpio_num_t(pin)); }
class NativeConsole {
 public:
  void begin(unsigned) {}
  size_t write(const uint8_t *data,size_t length) { return fwrite(data,1,length,stdout); }
  void println(const char *text) { puts(text); }
};
inline NativeConsole Serial;
class NativeChip {
 public:
  size_t getPsramSize() const;
  size_t getFreeHeap() const;
  uint64_t getEfuseMac() const;
  void restart() const;
};
inline NativeChip ESP;
// Channel-based PWM retained for the application; implemented using IDF LEDC.
bool ledcSetup(uint8_t channel,uint32_t hz,uint8_t resolution);
bool ledcAttachPin(int pin,uint8_t channel);
void ledcDetachPin(int pin);
void ledcWrite(uint8_t channel,uint32_t duty);
void configTime(long,long,const char *,const char *,const char *);
bool platformInit();

bool setCpuFrequencyMhz(uint32_t mhz);
