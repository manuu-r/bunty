#include "Log.h"
#include "Touch.h"
#include "TouchPress.h"

#include <Wire.h>
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

bool write(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readRegisters(uint8_t start, uint8_t *out, size_t length) {
  Wire.beginTransmission(kAddress);
  Wire.write(start);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(kAddress), static_cast<int>(length)) !=
      static_cast<int>(length)) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) out[i] = Wire.read();
  return true;
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
    vTaskDelayUntil(&tick, pdMS_TO_TICKS(kSampleMs));
  }
}

}  // namespace

bool begin() {
  if (cfg::pin::kTouchSda < 0) return false;
  Wire.begin(cfg::pin::kTouchSda, cfg::pin::kTouchScl, kBusHz);
  Wire.setTimeOut(20);
  if (cfg::pin::kTouchInterrupt >= 0) pinMode(cfg::pin::kTouchInterrupt, INPUT_PULLUP);

  Wire.beginTransmission(kAddress);
  present_ = Wire.endTransmission() == 0;
  if (!present_) {
    logPrintf("[touch] nothing at 0x%02x on SDA=%d SCL=%d\n", kAddress,
              cfg::pin::kTouchSda, cfg::pin::kTouchScl);
    for (uint8_t address = 0x08; address < 0x78; ++address) {
      Wire.beginTransmission(address);
      if (Wire.endTransmission() == 0) logPrintf("[touch] but 0x%02x answered\n", address);
    }
    return false;
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
