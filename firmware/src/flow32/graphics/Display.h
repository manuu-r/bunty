#pragma once

#include <Adafruit_GFX.h>
#include "Runtime.h"

#include "flow32/graphics/DisplayPanel.h"
#include "flow32/graphics/DisplayTransport.h"

/**
 * Smooth RGB565 display.
 *
 * Drawing goes into an off-screen framebuffer (PSRAM when available), so a
 * screen arrives in one present() instead of tearing in shape by shape.
 * present() transfers the whole panel or one rectangle through the transport.
 *
 * Everything Adafruit_GFX draws -- lines, circles, text -- routes through the
 * four primitives overridden below. fillRoundRect additionally anti-aliases its
 * corners, which is what gives Bunty's eyes their soft edges.
 */
class Display : public Adafruit_GFX {
public:
  Display(const DisplayPanel &panel, DisplayTransport &transport);
  ~Display();

  Display(const Display &) = delete;
  Display &operator=(const Display &) = delete;

  bool begin();
  void end();
  bool ready() const { return (fb_ || mono_) && line_ && transportBegun_; }
  void reportMemory() const;

  const DisplayPanel &panel() const { return panel_; }
  void setBacklight(bool on);

  // Transfer-only border overlay. Base framebuffer pixels remain unchanged;
  // zero opacity restores the original edges. Caller holds the UI mutex.
  void setAura(uint16_t color,uint8_t opacity);
  void present();
  void present(int16_t x, int16_t y, int16_t w, int16_t h);

  /** Fills the whole framebuffer without presenting it. */
  void clear(uint16_t color = 0);

  int16_t width() const { return panel_.width; }
  int16_t height() const { return panel_.height; }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override;
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override;
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                uint16_t color) override;

  /**
   * Anti-aliased rounded rect fill (soft corners via coverage blend).
   * Hides Adafruit_GFX::fillRoundRect when called on Display.
   */
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                     uint16_t color);

  /**
   * Copies `count` finished pixels into row `y` starting at `x`, clipped to
   * the panel. For renderers that compute a whole row at a time: one pass,
   * without drawPixel's per-pixel read-compare-write.
   */
  void writeRow(int16_t x, int16_t y, const uint16_t *colors, int16_t count);

private:
  size_t bufferBytes() const;
  bool transferRegion(int16_t x,int16_t y,int16_t w,int16_t h);
  uint16_t auraPixel(int16_t x,int16_t y,uint16_t base) const;
  void blendPixel(int16_t x, int16_t y, uint16_t fg, uint8_t cover4);
  uint16_t pixel(int16_t x, int16_t y) const;
  void markDirty(int16_t x, int16_t y);

  DisplayPanel panel_;
  DisplayTransport &transport_; // application-owned hardware boundary
  bool transportBegun_ = false;
  uint16_t *fb_ = nullptr;
  uint8_t *mono_ = nullptr;
  static constexpr int kTransferRows = 8;
  uint16_t *line_ = nullptr;
  // 16x16 tiles for the fixed 240x320 panel; no allocations while drawing.
  uint8_t dirty_[300] = {};
  uint16_t auraColor_=0;
  uint8_t auraOpacity_=0;
  bool auraPending_=false;
  uint32_t transferredPixels_ = 0;
  uint32_t transfers_ = 0;
};
