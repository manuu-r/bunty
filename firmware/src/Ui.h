#pragma once

#include <Arduino.h>
#include <gfxfont.h>

class Display;

// Owns the panel, its off-screen framebuffer, the backlight, and every static
// screen Bunty shows. Dashboard pages and Bunty's animated face draw through
// display() with a Guard held; everything else here is a complete screen.
namespace ui {

// Brings up SPI, the ST7789, the framebuffer, and the PWM backlight (dark).
bool begin();
Display &display();

// BLE security callbacks run on a Bluedroid task while dashboard and animation
// rendering runs on Arduino's loop task. The framebuffer and SPI transport are
// serialized across those tasks; the lock is recursive so a complete screen can
// call helpers that also present a small protected region.
class Guard {
 public:
  Guard();
  ~Guard();
  Guard(const Guard &) = delete;
  Guard &operator=(const Guard &) = delete;

 private:
  bool locked_ = false;
};

void fadeBacklight(bool on);
void fadeBacklightTo(uint8_t duty);

// Text helpers. All measure with the given font and leave it selected.
uint16_t textWidth(const GFXfont *font, const char *text);
// Truncates on a UTF-8 boundary and appends an ellipsis if needed.
const char *fitText(const GFXfont *font, const char *value, uint16_t maximumWidth);
void drawText(const GFXfont *font, const char *text, int16_t x, int16_t y,
              uint16_t color);
void drawTextRight(const GFXfont *font, const char *text, int16_t right,
                   int16_t y, uint16_t color);
void drawCentered(const GFXfont *font, const char *text, int16_t top,
                  uint16_t color);

// The persistent status bar: pairing state on the left, Wi-Fi on the right.
void drawTopBar(bool present);

void showPairing();
// Advances the pairing screen's activity dots; a no-op elsewhere.
void servicePairingActivity();
bool pairingVisible();

void showNumericComparison(uint32_t pin);
void showPairingConfirmed();
void showError(const char *message);
void showUnpaired();
void showSyncing(bool failed = false);
void drawControls();
// Draws where the panel thinks the finger landed, so a mirrored axis is
// visible on the device instead of only over a serial cable.
void drawTouchMarker(int16_t x, int16_t y);

bool settingsControlAt(int16_t x, int16_t y);
bool voiceControlAt(int16_t x, int16_t y);

struct SettingsStatus {
  bool paired = false;
  bool online = false;
  const char *network = "";
  const char *server = "";
  int32_t signal = 0;
};

enum class SettingsRow : uint8_t { kNone, kPair, kDisconnect, kUnpair, kClose };

void showSettings(const SettingsStatus &status);
SettingsRow settingsRowAt(int16_t x, int16_t y);

// Two large buttons; settingsRowAt is not used for these.
void showConfirm(const char *title, const char *detail);
bool confirmAcceptedAt(int16_t x, int16_t y);
bool confirmRejectedAt(int16_t x, int16_t y);

}  // namespace ui
