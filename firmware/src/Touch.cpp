#include "Log.h"
#include "Touch.h"
#include "TouchPress.h"
#include "Memory.h"

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "Config.h"

namespace touch {
namespace {

using namespace cfg::touch;

bool present_ = false;
PressTracker press_;
QueueHandle_t events_ = nullptr;
StaticQueue_t queueControl_;
uint8_t queueStorage_[8 * sizeof(Event)];
uint32_t lastConfiguredAt_ = 0;
int16_t rawX_ = 0;
int16_t rawY_ = 0;
int16_t seenMaxX_ = 0;
int16_t seenMaxY_ = 0;
bool reportedRange_ = false;

i2c_master_bus_handle_t bus_ = nullptr;
i2c_master_dev_handle_t device_ = nullptr;

bool write(uint8_t reg, uint8_t value) {
  const uint8_t bytes[] = {reg, value};
  return i2c_master_transmit(device_, bytes, sizeof(bytes), 20) == ESP_OK;
}

bool readRegisters(uint8_t start, uint8_t *out, size_t length) {
  return i2c_master_transmit_receive(device_, &start, 1, out, length, 20) == ESP_OK;
}

// The controller powers up in auto-sleep and its reset line is tied to the
// panel's on this board, so once it dozes nothing can wake it and every touch
// reads as no contact. Re-asserted periodically: a bus glitch restores the
// default.
void configure() {
  write(0xfe, 0x01);
  lastConfiguredAt_ = millis();
}

// -1 means the bus failed, 0 no contact, 1 a contact in *x and *y.
int contact(int16_t *x, int16_t *y) {
  uint8_t frame[6] = {};
  if (!readRegisters(0x01, frame, sizeof(frame))) return -1;
  if ((frame[1] & 0x0f) == 0) return 0;

  int16_t rawX = static_cast<int16_t>(((frame[2] & 0x0f) << 8) | frame[3]);
  int16_t rawY = static_cast<int16_t>(((frame[4] & 0x0f) << 8) | frame[5]);
  if (rawX > seenMaxX_) seenMaxX_ = rawX;
  if (rawY > seenMaxY_) seenMaxY_ = rawY;
  rawX_ = rawX;
  rawY_ = rawY;

  if (kSwapXY) {
    const int16_t swap = rawX;
    rawX = rawY;
    rawY = swap;
  }
  if (kInvertX) rawX = cfg::ui::kScreenWidth - 1 - rawX;
  if (kInvertY) rawY = cfg::ui::kScreenHeight - 1 - rawY;

  // Clamped, never rejected: a panel reporting slightly outside the frame, or
  // in an unexpected orientation, must still deliver usable touches instead of
  // silently dropping every one.
  *x = constrain(rawX, 0, cfg::ui::kScreenWidth - 1);
  *y = constrain(rawY, 0, cfg::ui::kScreenHeight - 1);
  return 1;
}

Event sample() {
  const uint32_t now = millis();
  if (now - lastConfiguredAt_ >= kReconfigureMs) configure();

  // INT is low only while a finger is down, so an idle panel costs no bus
  // traffic and cannot be mistaken for a stuck contact.
  if (cfg::pin::kTouchInterrupt >= 0 && digitalRead(cfg::pin::kTouchInterrupt)) {
    return press_.sample(0, 0, 0, now, kLongPressMs, kDebounceMs, kSlopPixels);
  }

  int16_t x = 0;
  int16_t y = 0;
  const int state = contact(&x, &y);
  if (!reportedRange_ && state == 1 &&
      (seenMaxX_ >= cfg::ui::kScreenWidth || seenMaxY_ >= cfg::ui::kScreenHeight)) {
    reportedRange_ = true;
    logPrintf("[touch] reported %d,%d beyond the %dx%d panel; check kSwapXY\n",
              seenMaxX_, seenMaxY_, cfg::ui::kScreenWidth, cfg::ui::kScreenHeight);
  }
  return press_.sample(state, x, y, now, kLongPressMs, kDebounceMs, kSlopPixels);
}

void readerTask(void *) {
  TickType_t tick = xTaskGetTickCount();
  for (;;) {
    const Event event = sample();
    if (event.kind != Event::Kind::kNone) {
      logPrintf("[touch] %s raw %d,%d -> %d,%d\n",
                event.kind == Event::Kind::kTap ? "tap" : "hold", rawX_, rawY_,
                event.x, event.y);
      xQueueSend(events_, &event, 0);
    }
    memory::sampleTask("touch");
    vTaskDelayUntil(&tick, pdMS_TO_TICKS(kSampleMs));
  }
}

}  // namespace

bool begin() {
  if(present_) return true;
  if (cfg::pin::kTouchSda < 0) return false;
  i2c_master_bus_config_t config = {};
  config.i2c_port = I2C_NUM_0;
  config.sda_io_num = gpio_num_t(cfg::pin::kTouchSda);
  config.scl_io_num = gpio_num_t(cfg::pin::kTouchScl);
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&config, &bus_) != ESP_OK) return false;
  if (cfg::pin::kTouchInterrupt >= 0) pinMode(cfg::pin::kTouchInterrupt, INPUT_PULLUP);
  present_ = i2c_master_probe(bus_, kAddress, 20) == ESP_OK;
  if (!present_) {
    logPrintf("[touch] nothing at 0x%02x on SDA=%d SCL=%d\n", kAddress,
              cfg::pin::kTouchSda, cfg::pin::kTouchScl);
    for (uint8_t address = 0x08; address < 0x78; ++address) {
      if (i2c_master_probe(bus_, address, 20) == ESP_OK)
        logPrintf("[touch] but 0x%02x answered\n", address);
    }
    i2c_del_master_bus(bus_);
    bus_ = nullptr;
    return false;
  }
  i2c_device_config_t device = {};
  device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  device.device_address = kAddress;
  device.scl_speed_hz = kBusHz;
  if (i2c_master_bus_add_device(bus_, &device, &device_) != ESP_OK) {
    i2c_del_master_bus(bus_); bus_=nullptr; present_=false; return false;
  }

  configure();
  uint8_t identity[3] = {};
  readRegisters(0xa7, identity, sizeof(identity));
  logPrintf("[touch] chip 0x%02x firmware %u, auto-sleep off\n", identity[0],
            static_cast<unsigned>(identity[2]));

  events_ = xQueueCreateStatic(8, sizeof(Event), queueStorage_, &queueControl_);
  if (!events_ || xTaskCreatePinnedToCore(readerTask, "touch", 2560, nullptr, 2,
                                          nullptr, 1) != pdPASS) {
    Serial.println("[touch] could not start the reader task");
    present_ = false;
    if(events_) { vQueueDelete(events_); events_=nullptr; }
    i2c_master_bus_rm_device(device_); device_=nullptr;
    i2c_del_master_bus(bus_); bus_=nullptr;
  }
  return present_;
}

bool available() { return present_; }

Event poll() {
  Event event;
  if (events_) xQueueReceive(events_, &event, 0);
  return event;
}

void flush() {
  if (events_) xQueueReset(events_);
}

}  // namespace touch
