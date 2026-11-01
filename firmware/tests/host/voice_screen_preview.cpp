// Renders every emotion's face through the real Display framebuffer and writes
// a PPM contact sheet, so faces can be judged without flashing the board.
#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Face.h"
#include "VoiceAura.h"
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

int main(int argc,char **argv) {
  if(argc!=2)return 2;
  constexpr uint16_t bg=((6&0xf8)<<8)|((8&0xfc)<<3)|(15>>3);
  std::vector<uint8_t> sheet(720*320*3);
  for(int state=0;state<3;++state){
    Panel panel;DisplayPanel spec;spec.width=240;spec.height=320;
    Display display(spec,panel);if(!display.begin())return 1;display.clear(bg);
    Face face;face.setAttention(state==0?Face::Attention::Listening:
      state==1?Face::Attention::Thinking:Face::Attention::Speaking);face.reset(0);
    for(uint32_t t=0;t<=700;t+=80) {
      face.drawEyes(display,t,bg);face.drawMouth(display,state==2?.65f:0.f,t,bg);
    }
    const auto aura=voiceAura::frame(state==0?voiceAura::State::Listening:
      state==1?voiceAura::State::Thinking:voiceAura::State::Speaking,700,state==2?65:0);
    display.setAura(aura.color,aura.opacity);
    display.present();
    assert(panel.screen[155*240+55]!=bg); // each state retains its eyes
    for(int y=0;y<320;++y)for(int x=0;x<240;++x){
      const uint16_t color=panel.screen[y*240+x];
      auto *out=&sheet[(y*720+state*240+x)*3];
      out[0]=((color>>11)&31)*255/31;out[1]=((color>>5)&63)*255/63;out[2]=(color&31)*255/31;
    }
  }
  FILE *out=fopen(argv[1],"wb");if(!out)return 1;
  fprintf(out,"P6\n720 320\n255\n");fwrite(sheet.data(),1,sheet.size(),out);fclose(out);return 0;
}
