#include "Log.h"
#include "Ui.h"

#include <WiFi.h>
#include <freertos/semphr.h>

#include "Config.h"
#include "Fonts.h"
#include "flow32/graphics/Display.h"
#include "flow32/graphics/St77xxTransport.h"

namespace ui {
namespace {

using namespace cfg::ui;

DisplayPanel makePanel() {
  DisplayPanel panel;
  panel.id = "bunty-st7789";
  panel.width = kScreenWidth;
  panel.height = kScreenHeight;
  return panel;
}

St77xxConfig makeTransport() {
  St77xxConfig config;
  config.spi = &SPI;
  config.pinCs = cfg::pin::kTftCs;
  config.pinDc = cfg::pin::kTftDc;
  config.pinRst = cfg::pin::kTftRst;
  config.pinMosi = cfg::pin::kTftMosi;
  config.pinSclk = cfg::pin::kTftSclk;
  config.gramWidth = kScreenWidth;
  config.gramHeight = kScreenHeight;
  config.spiHz = kSpiHz;
  return config;
}

const DisplayPanel kPanel = makePanel();
St77xxTransport transport(makeTransport());

// Flow32's Display is an Adafruit_GFX that paints into an off-screen
// framebuffer, so a screen arrives in one present() instead of tearing in
// shape by shape.
Display panel(kPanel, transport);
SemaphoreHandle_t renderMutex = nullptr;
StaticSemaphore_t renderMutexStorage;

uint8_t backlightDuty = 0;
bool pairingVisible_ = false;
uint32_t lastActivityAt = 0;
uint8_t activityFrame = 0;

constexpr int16_t kActivityLeft = 94;
constexpr int16_t kActivityTop = 254;
constexpr int16_t kActivityWidth = 52;
constexpr int16_t kActivityHeight = 20;

// Bluetooth's rune: a spine, a flag at each end, and the two strokes that
// cross it.
void drawBluetoothIcon(int16_t cx, int16_t cy, uint16_t color) {
  constexpr int16_t w = 3;
  constexpr int16_t h = 5;
  panel.drawLine(cx, cy - h, cx, cy + h, color);
  panel.drawLine(cx, cy - h, cx + w, cy - h / 2, color);
  panel.drawLine(cx + w, cy - h / 2, cx - w, cy + h / 2, color);
  panel.drawLine(cx, cy + h, cx + w, cy + h / 2, color);
  panel.drawLine(cx + w, cy + h / 2, cx - w, cy - h / 2, color);
}

// Three arcs over a dot. Adafruit's quarter-circle helper draws the top half
// with the top-left and top-right corner bits set.
void drawWifiIcon(int16_t cx, int16_t baseY, uint16_t color) {
  for (int16_t r = 2; r <= 6; r += 2) {
    panel.drawCircleHelper(cx, baseY, r, 0x1 | 0x2, color);
  }
  panel.fillCircle(cx, baseY, 1, color);
}

void drawActivityDots() {
  Guard guard;
  panel.fillRect(kActivityLeft, kActivityTop, kActivityWidth, kActivityHeight,
                 kBackground);
  for (uint8_t i = 0; i < 3; ++i) {
    const bool lit = i == activityFrame;
    panel.fillCircle(106 + i * 14, 264, lit ? 4 : 3, lit ? kAccent : kBorder);
  }
  panel.present(kActivityLeft, kActivityTop, kActivityWidth, kActivityHeight);
}

}  // namespace

Guard::Guard() {
  if (renderMutex) {
    locked_ = xSemaphoreTakeRecursive(renderMutex, portMAX_DELAY) == pdTRUE;
  }
}

Guard::~Guard() {
  if (locked_) xSemaphoreGiveRecursive(renderMutex);
}

Display &display() { return panel; }

bool begin() {
  renderMutex = xSemaphoreCreateRecursiveMutexStatic(&renderMutexStorage);
  if (!renderMutex) return false;

  ledcSetup(kBacklightChannel, kBacklightPwmHz, 8);
  ledcAttachPin(cfg::pin::kTftBacklight, kBacklightChannel);
  ledcWrite(kBacklightChannel, 0);

  // The transport initializes SPI; Display then claims ~150 KB of PSRAM for
  // its framebuffer before Wi-Fi and BLE take their share of the heap.
  if (!panel.begin()) {
    Serial.println("[display] could not allocate the framebuffer");
    return false;
  }
  panel.clear(kBackground);
  return true;
}

void fadeBacklightTo(uint8_t targetDuty) {
  constexpr uint8_t kSteps = 28;
  const int16_t startDuty = backlightDuty;
  if (startDuty == targetDuty) return;
  for (uint8_t step = 1; step <= kSteps; ++step) {
    const int16_t duty =
        startDuty +
        (static_cast<int16_t>(targetDuty) - startDuty) * step / kSteps;
    ledcWrite(kBacklightChannel, static_cast<uint8_t>(duty));
    delay(targetDuty > startDuty ? 8 : 5);
  }
  backlightDuty = targetDuty;
}

void fadeBacklight(bool on) { fadeBacklightTo(on ? 255 : 0); }

uint16_t textWidth(const GFXfont *font, const char *text) {
  panel.setFont(font);
  panel.setTextSize(1);
  panel.setTextWrap(false);
  int16_t x = 0;
  int16_t y = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  panel.getTextBounds(text, 0, 0, &x, &y, &width, &height);
  return width;
}

const char *fitText(const GFXfont *font, const char *value,
               uint16_t maximumWidth) {
  // Callers hold Guard and consume the result before another fitText call.
  static char shortened[256];
  const size_t length = strlcpy(shortened, value, sizeof(shortened) - 3);
  if (length < sizeof(shortened) - 3 && textWidth(font, shortened) <= maximumWidth) return shortened;
  size_t size = strlen(shortened);
  while (size > 0) {
    size_t start = size - 1;
    // Step back over UTF-8 continuation bytes so a multi-byte character is
    // removed whole.
    while (start > 0 &&
           (static_cast<uint8_t>(shortened[start]) & 0xc0) == 0x80) {
      --start;
    }
    size = start;
    memcpy(shortened + size, "...", 4);
    if (textWidth(font, shortened) <= maximumWidth) return shortened;
  }
  return "...";
}

void drawText(const GFXfont *font, const char *text, int16_t x, int16_t y,
              uint16_t color) {
  panel.setFont(font);
  panel.setTextSize(1);
  panel.setTextColor(color);
  panel.setTextWrap(false);
  panel.setCursor(x, y);
  panel.print(text);
}

void drawTextRight(const GFXfont *font, const char *text, int16_t right,
                   int16_t y, uint16_t color) {
  panel.setFont(font);
  panel.setTextSize(1);
  panel.setTextWrap(false);
  int16_t xOffset = 0;
  int16_t yOffset = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  panel.getTextBounds(text, 0, 0, &xOffset, &yOffset, &width, &height);
  drawText(font, text, right - static_cast<int16_t>(width) - xOffset, y, color);
}

void drawCentered(const GFXfont *font, const char *text, int16_t top,
                  uint16_t color) {
  panel.setFont(font);
  panel.setTextSize(1);
  panel.setTextColor(color);
  panel.setTextWrap(false);
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  panel.getTextBounds(text, 0, 0, &x1, &y1, &width, &height);
  panel.setCursor((kScreenWidth - static_cast<int16_t>(width)) / 2 - x1,
                  top - y1);
  panel.print(text);
}

void drawTopBar(bool present) {
  const bool online = WiFi.status() == WL_CONNECTED;
  panel.fillRect(0, 0, kScreenWidth, kStatusBarHeight, kSurface);
  drawBluetoothIcon(14, 16, kSuccess);
  drawText(BuntyFonts::kBody, "PAIRED", 25, 21, kSuccess);
  drawWifiIcon(149, 20, online ? kSuccess : kBorder);
  drawText(BuntyFonts::kBody, online ? "ONLINE" : "OFFLINE", 162, 21,
           online ? kSuccess : kMuted);
  panel.drawFastHLine(0, kStatusBarHeight - 1, kScreenWidth, kBorder);
  if (present) panel.present(0, 0, kScreenWidth, kStatusBarHeight);
}

void showPairing() {
  Guard guard;
  panel.fillScreen(kBackground);
  // Keep every line inside a 216 px safe area. An 18 pt "PAIR IN GAUGE"
  // heading measured 268 px wide on this 240 px panel.
  drawCentered(BuntyFonts::kHeading, "PAIR BUNTY", 72, kText);
  drawCentered(BuntyFonts::kBody, "Open Gauge on your Mac", 133, kMuted);
  drawCentered(BuntyFonts::kBody, "Choose Pair Accessory", 165, kMuted);
  drawCentered(BuntyFonts::kBody, "Then compare the code", 197, kMuted);
  drawActivityDots();
  panel.present();
  pairingVisible_ = true;
}

void servicePairingActivity() {
  if (!pairingVisible_) return;
  const uint32_t now = millis();
  if (now - lastActivityAt < cfg::pairing::kActivityFrameMs) return;
  lastActivityAt = now;
  activityFrame = (activityFrame + 1) % 3;
  drawActivityDots();
}

bool pairingVisible() { return pairingVisible_; }

void showNumericComparison(uint32_t pin) {
  pairingVisible_ = false;
  Guard guard;
  char digits[7];
  snprintf(digits, sizeof(digits), "%06lu", static_cast<unsigned long>(pin));
  panel.fillScreen(kBackground);
  drawCentered(BuntyFonts::kBody, "DOES THE MAC SHOW", 66, kMuted);
  drawCentered(BuntyFonts::kComparison, digits, 112, kText);
  drawCentered(BuntyFonts::kBody, "1 TAP  YES", 197, kSuccess);
  drawCentered(BuntyFonts::kBody, "2 TAPS  NO", 229, kDanger);
  panel.present();
}

void showPairingConfirmed() {
  pairingVisible_ = false;
  Guard guard;
  panel.fillScreen(kBackground);
  drawCentered(BuntyFonts::kHeading, "CONFIRMED", 122, kSuccess);
  drawCentered(BuntyFonts::kBody, "Finishing secure setup", 174, kMuted);
  panel.present();
}

void showError(const char *message) {
  pairingVisible_ = false;
  Guard guard;
  panel.fillScreen(kBackground);
  drawCentered(BuntyFonts::kHeading, "Try again", 132, kDanger);
  drawCentered(BuntyFonts::kBody, message, 176, kMuted);
  panel.present();
  logPrintf("[pairing] %s\n", message);
}

void showUnpairPrompt() {
  Guard guard;
  panel.fillScreen(kBackground);
  drawTopBar(false);
  drawCentered(BuntyFonts::kHeading, "UNPAIR?", 76, kText);
  drawCentered(BuntyFonts::kBody, "Bunty will forget this Mac", 139, kMuted);
  drawCentered(BuntyFonts::kBody, "Pause 1 sec, then tap", 171, kSuccess);
  drawCentered(BuntyFonts::kBody, "Wait 6 seconds to cancel", 203, kMuted);
  panel.present();
}

void showUnpaired() {
  Guard guard;
  panel.fillScreen(kBackground);
  drawCentered(BuntyFonts::kHeading, "UNPAIRED", 121, kSuccess);
  drawCentered(BuntyFonts::kBody, "Restarting in pairing mode", 174, kMuted);
  panel.present();
}

void showSyncing() {
  Guard guard;
  panel.fillScreen(kBackground);
  drawTopBar(false);
  drawCentered(BuntyFonts::kHeading, "SYNCING", 91, kText);
  drawCentered(BuntyFonts::kBody, "Fetching Gauge stats", 158, kMuted);
  drawCentered(BuntyFonts::kBody, "Will retry automatically", 194, kMuted);
  panel.present();
}

}  // namespace ui
