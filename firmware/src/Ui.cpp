#include "Log.h"
#include "Ui.h"

#include <WiFi.h>
#include <freertos/semphr.h>

#include "Config.h"
#include "Fonts.h"
#include "Power.h"
#include "RuntimeTiming.h"
#include "MuseText.h"
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

constexpr int16_t kConfirmTop = 150;
constexpr int16_t kConfirmHeight = 66;
constexpr int16_t kRejectTop = 228;

uint8_t backlightDuty = 0;
uint8_t backlightRequested = 0;
runtime::Fade backlightFade;
bool pairingVisible_ = false;
uint32_t lastActivityAt = 0;
uint8_t activityFrame = 0;

constexpr int16_t kActivityLeft = 94;
constexpr int16_t kActivityTop = 254;
constexpr int16_t kActivityWidth = 52;
constexpr int16_t kActivityHeight = 20;

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

  if(!ledcSetup(kBacklightChannel, kBacklightPwmHz, 8) ||
     !ledcAttachPin(cfg::pin::kTftBacklight, kBacklightChannel)) return false;
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
  Guard guard;
  backlightRequested = targetDuty;
  serviceBacklight();
}

void serviceBacklight() {
  Guard guard;
  const uint32_t now=millis();
  backlightFade.target(std::min(backlightRequested,power::backlightCeiling()),now);
  const auto duty=backlightFade.sample(now);
  if(duty!=backlightDuty) { ledcWrite(kBacklightChannel,duty); backlightDuty=duty; }
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

void showPairing() {
  Guard guard;
  panel.fillScreen(kBackground);
  // Keep every line inside the 216 px safe area.
  drawCentered(BuntyFonts::kHeading, "Pair Bunty", 72, kText);
  drawCentered(BuntyFonts::kBody, "Open Muse > Devices", 133, kMuted);
  drawCentered(BuntyFonts::kBody, "Enable Developer mode", 165, kMuted);
  drawCentered(BuntyFonts::kBody, "Confirm pairing here", 197, kMuted);
  drawActivityDots();
  panel.present();
  pairingVisible_ = true;
}

void servicePairingActivity() {
  if (!pairingVisible_) return;
  const uint32_t now = millis();
  if (now - lastActivityAt < cfg::ui::kPairingActivityFrameMs) return;
  lastActivityAt = now;
  activityFrame = (activityFrame + 1) % 3;
  drawActivityDots();
}

bool pairingVisible() { return pairingVisible_; }

void showPairingConfirmed() {
  pairingVisible_ = false;
  Guard guard;
  panel.fillScreen(kBackground);
  drawCentered(BuntyFonts::kHeading, "Paired", 122, kSuccess);
  drawCentered(BuntyFonts::kBody, "Finishing secure setup", 174, kMuted);
  panel.present();
}

void showError(const char *message) {
  pairingVisible_ = false;
  Guard guard;
  panel.fillScreen(kBackground);
  muse::textPage(message,0,208,3,
    [](const char *line){return textWidth(BuntyFonts::kBody,line);},
    [](size_t row,const char *line){drawText(BuntyFonts::kBody,line,16,144+int(row)*22,kMuted);});
  panel.present();
  logPrintf("[pairing] %s\n", message);
}

namespace {

bool settingsPaired_ = false;
constexpr int16_t kRowHeight = 50;
constexpr int16_t kFirstRowTop = 74;

int16_t rowTop(uint8_t index) { return kFirstRowTop + index * kRowHeight; }

void drawRow(uint8_t index, const char *label, const char *value,
             uint16_t accent, bool enabled) {
  const int16_t top = rowTop(index);
  panel.drawFastHLine(16,top+kRowHeight-8,208,kBorder);
  drawText(BuntyFonts::kBody,fitText(BuntyFonts::kBody,label,value && *value?126:200),20,top+20,enabled?accent:kMuted);
  if (value && *value) {
    drawTextRight(BuntyFonts::kBody, value, kScreenWidth - 24, top + 20, kMuted);
  }
}

}  // namespace

void showSettings(const SettingsStatus &status) {
  settingsPaired_ = status.paired;
  Guard guard;
  panel.fillScreen(kBackground);
  drawCentered(BuntyFonts::kHeading, "Settings", 24, kText);

  if (status.paired) {
    char signal[24];
    snprintf(signal, sizeof(signal), "%ld dBm", static_cast<long>(status.signal));
    drawRow(0, status.online ? "Connected" : "Offline", status.online ? signal : "",
            status.online ? kSuccess : kDanger, true);
    drawRow(1, status.network[0] ? status.network : "No network", "", kMuted, true);
    drawRow(2, "Disconnect", "", kText, true);
    drawRow(3, "Unpair", "", kDanger, true);
  } else {
    drawRow(0, "Not paired", "", kMuted, true);
    drawRow(1, "Pair with Muse", "", kAccent, true);
  }
  drawCentered(BuntyFonts::kBody, "Tap outside to return", 296, kMuted);
  panel.present();
}

SettingsRow settingsRowAt(int16_t x, int16_t y) {
  if (x < 10 || x > kScreenWidth - 10) return SettingsRow::kClose;
  for (uint8_t index = 0; index < 4; ++index) {
    const int16_t top = rowTop(index);
    if (y < top || y >= top + kRowHeight - 8) continue;
    switch (index) {
      case 1: return settingsPaired_ ? SettingsRow::kNone : SettingsRow::kPair;
      case 2: return settingsPaired_ ? SettingsRow::kDisconnect : SettingsRow::kClose;
      case 3: return settingsPaired_ ? SettingsRow::kUnpair : SettingsRow::kClose;
      default: return SettingsRow::kNone;
    }
  }
  return SettingsRow::kClose;
}

void showConfirm(const char *title, const char *detail) {
  Guard guard;
  panel.fillScreen(kBackground);
  drawCentered(BuntyFonts::kHeading, title, 48, kText);
  if (detail && *detail) drawCentered(BuntyFonts::kBody, detail, 108, kMuted);

  panel.drawFastHLine(20,kConfirmTop+kConfirmHeight,kScreenWidth-40,kBorder);
  drawCentered(BuntyFonts::kBody, "Confirm", kConfirmTop + 24, kText);

  drawCentered(BuntyFonts::kBody, "Cancel", kRejectTop + 24, kText);
  panel.present();
}

bool confirmAcceptedAt(int16_t x, int16_t y) {
  return x >= 20 && x <= kScreenWidth - 20 && y >= kConfirmTop &&
         y < kConfirmTop + kConfirmHeight;
}

bool confirmRejectedAt(int16_t x, int16_t y) {
  return x >= 20 && x <= kScreenWidth - 20 && y >= kRejectTop &&
         y < kRejectTop + kConfirmHeight;
}

// The quiet handle opens requested content; the face is a full-size Talk target.
bool settingsControlAt(int16_t x,int16_t y) {return x>=72 && x<168 && y>=266;}
void drawControls() {
  panel.fillRect(0,266,kScreenWidth,54,kBackground);
  panel.fillRoundRect(100,302,40,3,1,kMuted);
}

} // namespace ui
