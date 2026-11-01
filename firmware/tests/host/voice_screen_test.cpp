#include "VoiceScreen.h"
#include "VoiceTransition.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
class Raster:public Adafruit_GFX {
 public:
  int top=0,bottom=320;
  Raster():Adafruit_GFX(240,320){}
  void drawPixel(int16_t x,int16_t y,uint16_t)override {assert(x>=0 && x<240 && y>=top && y<bottom);}
};
int main(){
  Raster g;g.top=32;g.bottom=96;
  for(const char *s:{"Happy","Curious","Sleepy"})voiceScreen::heading(g,s);
  assert(voiceScreen::transition(100,100)==0 && voiceScreen::transition(420,100)==1);
  assert(voiceScreen::transition(260,100)==.5f);
  assert(voiceScreen::transition(49,UINT32_MAX-50)>0);
  assert(voiceScreen::opacity(-1)==0 && voiceScreen::opacity(2)==255);
  puts("Voice screen: expression labels, fade limits and transition clock wrap passed");
}
