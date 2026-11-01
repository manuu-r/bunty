#include "MuseScreenBuffer.h"
#include "MuseText.h"
#include "Fonts.h"
#include <Adafruit_GFX.h>
#include <array>
#include <cassert>
#include <climits>
#include <iostream>
#include <string>
#include <vector>

class Measure : public Adafruit_GFX {
 public:
  Measure() : Adafruit_GFX(240,320) { setFont(BuntyFonts::kBody); setTextWrap(false); }
  void drawPixel(int16_t,int16_t,uint16_t) override {}
  int width(const char *text) {
    int16_t x,y; uint16_t w,h; getTextBounds(text,0,0,&x,&y,&w,&h); return w;
  }
};
int main() {
  std::vector<uint16_t> pixels(240*320);
  muse::ScreenBuffer image; image.bind(pixels.data(),240,320);
  assert(image.begin(0x1234)); assert(!image.begin(0));
  const uint8_t colors[]={0xf8,0,0x07,0xe0,0,0x1f,0xff,0xff};
  assert(image.write(238,318,2,2,colors));
  assert(image.row(318)[238]==0xf800 && image.row(318)[239]==0x07e0);
  assert(image.row(319)[238]==0x001f && image.row(319)[239]==0xffff);
  assert(image.row(0)[0]==0x1234);
  for(const auto &r:std::array<std::array<int,4>,7>{{
      {{-1,0,1,1}},{{0,-1,1,1}},{{239,319,2,1}},{{0,319,1,2}},
      {{0,0,0,1}},{{0,0,INT_MAX,INT_MAX}},{{INT_MAX,0,1,1}}}})
    assert(!image.write(r[0],r[1],r[2],r[3],colors));
  assert(!image.write(0,0,1,1,nullptr));
  assert(image.finish(true)); assert(!image.current());
  assert(!image.write(0,0,1,1,colors));
  assert(image.begin(0)); image.cancel();
  assert(!image.write(0,0,1,1,colors)); assert(!image.finish(true));
  assert(image.begin(0)); assert(!image.finish(false));
  assert(image.begin(0)); assert(image.finish(true)); // retry after cancellation/failure

  char ascii[64];
  assert(muse::screenAscii("caf\xc3\xa9 \xf0\x9f\x99\x82\r\nA\tB",ascii,sizeof(ascii))==10);
  assert(std::string(ascii)=="caf? ?\nA B");
  char tiny[2]; muse::screenAscii("\xf0\x9f\x99\x82x",tiny,sizeof(tiny));
  assert(std::string(tiny)=="?");
  muse::screenAscii("abc",tiny,0); assert(std::string(tiny)=="?");
  uint16_t color=0;
  assert(muse::screenColor("#ff0000",color) && color==0xf800);
  assert(muse::screenColor("#00FF00",color) && color==0x07e0);
  assert(muse::screenColor("#0000ff",color) && color==0x001f);
  assert(!muse::screenColor("#GG0000",color));
  assert(!muse::screenColor("red",color)); assert(!muse::screenColor(nullptr,color));

  Measure font;
  std::string input;
  for(int i=0;i<90;++i) input+="Muse can show pictures and text. ";
  input+="\n\n"+std::string(400,'W'); // explicit blank lines and a very long word
  size_t at=0,pages=0,letters=0;
  while(at<input.size()) {
    const size_t next=muse::textPage(input.c_str(),at,216,10,
      [&](const char *line) { return font.width(line); },
      [&](size_t row,const char *line) {
        assert(row<10); assert(font.width(line)<=216);
        for(const char *p=line;*p;++p) if(*p!=' ') ++letters;
      });
    assert(next>at && next<=input.size()); at=next; ++pages;
  }
  size_t expected=0; for(char ch:input) if(ch!=' ' && ch!='\n') ++expected;
  assert(letters==expected && pages>1);
  std::vector<std::string> lines;
  const char *blank="a\n\n   \nb";
  assert(muse::textPage(blank,0,216,10,[&](const char *s) { return font.width(s); },
    [&](size_t,const char *s) { lines.emplace_back(s); })==std::strlen(blank));
  assert((lines==std::vector<std::string>{"a","","","b"}));
  std::cout << "Muse image bounds, RGB565 colour order, cancellation, text wrapping/paging and UTF-8 fallback passed\n";
}
