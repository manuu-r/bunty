// Renders every emotion's face through the real Display framebuffer and writes
// a PPM contact sheet, so faces can be judged without flashing the board.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Face.h"
#include "Fonts.h"
#include "flow32/graphics/Display.h"

extern "C" void *__real_malloc(size_t size) { return std::malloc(size); }
extern "C" void *__real_calloc(size_t count, size_t size) { return std::calloc(count, size); }
void *heap_caps_malloc(size_t bytes, uint32_t) { return std::malloc(bytes); }
void *heap_caps_calloc(size_t count, size_t size, uint32_t) { return std::calloc(count, size); }
void heap_caps_free(void *p) { std::free(p); }

struct Panel : DisplayTransport {
  std::vector<uint16_t> screen = std::vector<uint16_t>(240 * 320);
  bool begin(const DisplayPanel &) override { return true; }
  void end() override {}
  bool present(const uint16_t *data, int16_t x, int16_t y, int16_t w, int16_t h, int16_t stride) override {
    for (int row = 0; row < h; ++row)
      for (int col = 0; col < w; ++col) screen[(y + row) * 240 + x + col] = data[row * stride + col];
    return true;
  }
};

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "faces.ppm";
  const float open = argc > 2 ? std::atof(argv[2]) : 0.5f;
  const uint32_t at = argc > 3 ? std::atoi(argv[3]) : 700;
  constexpr uint16_t kBackground = ((6 & 0xF8) << 8) | ((8 & 0xFC) << 3) | (15 >> 3);
  constexpr int kEmotions=static_cast<int>(Emotion::Count),kCount=kEmotions+4,kCols=4;
  constexpr const char *names[]={"NEUTRAL","SOFT","HAPPY","EXCITED","FOCUSED","SAD","CURIOUS","SASSY","IDLE","LISTENING","THINKING","SPEAKING"};
  constexpr int kW=240,kH=280,kTop=32;
  const int rows = (kCount + kCols - 1) / kCols;
  std::vector<uint8_t> sheet(size_t(kW * kCols) * kH * rows * 3, 40);

  for (int e = 0; e < kCount; ++e) {
    Panel transport;
    DisplayPanel spec;
    spec.width = 240;
    spec.height = 320;
    Display display(spec, transport);
    if (!display.begin()) return 1;
    display.clear(kBackground);
    Face face;
    face.setEmotion(e<kEmotions?static_cast<Emotion>(e):Emotion::Neutral);
    if(e>=kEmotions)face.setAttention(static_cast<Face::Attention>(e-kEmotions));
    face.reset(0);
    // Advance in small steps, as the firmware does, so tweens and blinks run.
    for (uint32_t t = 0; t <= at; t += 60) {
      face.drawEyes(display, t, kBackground);
      face.drawMouth(display, e==kEmotions+3?.7f:open, t, kBackground);
    }
    display.setFont(BuntyFonts::kBody);display.setTextColor(uint16_t((142>>3)<<11|(151>>2)<<5|(172>>3)));
    display.setTextSize(1);display.setTextWrap(false);display.setCursor(16,60);display.print(names[e]);
    display.present();
    const int ox = (e % kCols) * kW, oy = (e / kCols) * kH;
    for (int y = 0; y < kH; ++y)
      for (int x = 0; x < kW; ++x) {
        const uint16_t c = transport.screen[(y + kTop) * 240 + x];
        uint8_t *p = &sheet[((size_t(oy + y) * kW * kCols) + ox + x) * 3];
        p[0] = ((c >> 11) & 31) * 255 / 31;
        p[1] = ((c >> 5) & 63) * 255 / 63;
        p[2] = (c & 31) * 255 / 31;
      }
  }
  FILE *file = std::fopen(path, "wb");
  std::fprintf(file, "P6\n%d %d\n255\n", kW * kCols, kH * rows);
  std::fwrite(sheet.data(), 1, sheet.size(), file);
  std::fclose(file);
  return 0;
}
