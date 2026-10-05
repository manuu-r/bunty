#include <Adafruit_GFX.h>
#include "Fonts.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

class Raster : public Adafruit_GFX {
 public:
  Raster() : Adafruit_GFX(240, 320), pixels(240 * 320) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x >= 0 && y >= 0 && x < 240 && y < 320) pixels[y * 240 + x] = color;
  }
  std::vector<uint16_t> pixels;
};
int main() {
  Raster raster;
  raster.setTextWrap(false);
  raster.setTextColor(0xffff);
  for (const auto *font : {BuntyFonts::kBody, BuntyFonts::kHeading, BuntyFonts::kComparison}) {
    raster.fillScreen(0);
    raster.setFont(font);
    const char *text = font == BuntyFonts::kComparison ? "123456" : "BUNTY";
    int16_t x, y;
    uint16_t w, h;
    raster.getTextBounds(text, 40, 80, &x, &y, &w, &h);
    assert(w > 0 && h > 0 && w < 200);
    raster.setCursor(40, 80);
    assert(raster.print(text) == std::strlen(text));
    size_t lit = 0;
    for (int row = 0; row < 320; ++row) for (int col = 0; col < 240; ++col) {
      if (!raster.pixels[row * 240 + col]) continue;
      assert(col >= x && col < x + w && row >= y && row < y + h);
      ++lit;
    }
    assert(lit > 10);
  }
  raster.fillScreen(0);
  raster.fillCircle(30, 30, 10, 0xabcd);
  assert(raster.pixels[30 * 240 + 30] == 0xabcd);
  assert(raster.pixels[30 * 240 + 41] == 0);
  std::cout << "Native GFX: Bunty fonts, text bounds, Print byte count and primitives passed\n";
}
