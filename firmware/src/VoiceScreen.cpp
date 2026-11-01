#include "VoiceScreen.h"
#include "Fonts.h"
#include <cstring>
namespace voiceScreen {
namespace {
constexpr uint16_t rgb(int r,int g,int b){return uint16_t((r>>3)<<11|(g>>2)<<5|(b>>3));}
constexpr auto bg=rgb(6,8,15);
void text(Adafruit_GFX &g,const char *value,int x,int y,uint16_t color){
  g.setFont(BuntyFonts::kBody);g.setTextSize(1);g.setTextWrap(false);g.setTextColor(color);g.setCursor(x,y);g.print(value);
}
void centered(Adafruit_GFX &g,const char *value,int center,int y,uint16_t color){
  g.setFont(BuntyFonts::kBody);g.setTextSize(1);
  int16_t x0,y0;uint16_t w,h;g.getTextBounds(value,0,0,&x0,&y0,&w,&h);
  text(g,value,center-int(w)/2-x0,y,color);
}
}
void heading(Adafruit_GFX &g,const char *title,uint8_t opacity){
  g.fillRect(0,32,240,64,bg);
  centered(g,title,120,82,ink(opacity));
}
uint16_t ink(uint8_t opacity){
  const auto mix=[&](int a,int b){return (a*opacity+b*(255-opacity)+127)/255;};
  return rgb(mix(233,6),mix(241,8),mix(223,15));
}

}
