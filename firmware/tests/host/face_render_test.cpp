#include "Face.h"
#include "flow32/graphics/Display.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <initializer_list>
static int allocations=0;
extern "C" void *__real_malloc(size_t size){return std::malloc(size);}
extern "C" void *__real_calloc(size_t n,size_t size){return std::calloc(n,size);}
void *heap_caps_malloc(size_t bytes,uint32_t){++allocations;return std::malloc(bytes);}
void *heap_caps_calloc(size_t n,size_t bytes,uint32_t){++allocations;return std::calloc(n,bytes);}
void heap_caps_free(void *p){std::free(p);}
struct Panel : DisplayTransport {
  uint16_t pixels[240*320]{};
  bool begin(const DisplayPanel &)override{return true;}
  void end()override{}
  bool present(const uint16_t *data,int16_t x,int16_t y,int16_t w,int16_t h,int16_t stride)override {
    assert(x>=0 && y>=0 && x+w<=240 && y+h<=320);
    for(int row=0;row<h;++row)for(int col=0;col<w;++col)pixels[(y+row)*240+x+col]=data[row*stride+col];
    return true;
  }
};
void present(Display &display,Face::Rect r,int top,int height){
  if(r.x1<=r.x0)return;
  assert(r.x0>=0 && r.x1<=240 && r.y0>=top && r.y1<=top+height);
  display.present(r.x0,r.y0,r.x1-r.x0,r.y1-r.y0);
}
int main(){
  Panel panel;DisplayPanel spec;spec.width=240;spec.height=320;
  Display display(spec,panel);assert(display.begin());
  constexpr uint16_t sentinel=0x3456,bg=0x0041;
  display.clear(sentinel);display.present();
  Face face;face.reset(0);const int before=allocations;
  for(int mood=0;mood<int(Emotion::Count);++mood){
    face.setEmotion(static_cast<Emotion>(mood));
    for(int state=0;state<4;++state){
      face.setAttention(static_cast<Face::Attention>(state));
      for(uint32_t t=0;t<3600;t+=80){
        const uint32_t now=uint32_t(mood*20000+state*4000)+t;
        present(display,face.drawEyes(display,now,bg),Face::kEyesTop,Face::kEyesHeight);
        present(display,face.drawMouth(display,(t%1000)/1000.f,now,bg),Face::kMouthTop,Face::kMouthHeight);
      }
    }
  }
  // Dirty bands must preserve captions, status, listening glyph and controls.
  for(int y=0;y<320;++y)for(int x=0;x<240;++x){
    if((y>=Face::kEyesTop && y<Face::kEyesTop+Face::kEyesHeight) ||
       (y>=Face::kMouthTop && y<Face::kMouthTop+Face::kMouthHeight))continue;
    assert(panel.pixels[y*240+x]==sentinel);
  }
  face.setEmotion(static_cast<Emotion>(255));face.reset(0);
  face.drawMouth(display,std::numeric_limits<float>::quiet_NaN(),80,bg);
  face.drawMouth(display,-50,160,bg);face.drawMouth(display,50,240,bg);
  face.setEmotion(Emotion::Neutral);face.reset(0);
  face.drawMouth(display,0,0,bg);
  auto held=face.drawMouth(display,0,80,bg);assert(held.x0==held.x1);
  // After speech, repeated silent draws must return to the closed pose and
  // erase the open mouth's previous footprint without touching captions.
  for(uint32_t t=160;t<=800;t+=80)
    present(display,face.drawMouth(display,1,t,bg),Face::kMouthTop,Face::kMouthHeight);
  for(uint32_t t=880;t<=2400;t+=80)
    present(display,face.drawMouth(display,0,t,bg),Face::kMouthTop,Face::kMouthHeight);
  uint16_t released[Face::kMouthWidth*Face::kMouthHeight];
  for(int y=0;y<Face::kMouthHeight;++y)for(int x=0;x<Face::kMouthWidth;++x)
    released[y*Face::kMouthWidth+x]=panel.pixels[(y+Face::kMouthTop)*240+x+Face::kMouthLeft];
  face.reset(0);
  present(display,face.drawMouth(display,0,0,bg),Face::kMouthTop,Face::kMouthHeight);
  for(int y=0;y<Face::kMouthHeight;++y)for(int x=0;x<Face::kMouthWidth;++x)
    assert(released[y*Face::kMouthWidth+x]==panel.pixels[(y+Face::kMouthTop)*240+x+Face::kMouthLeft]);
  face.reset(std::numeric_limits<uint32_t>::max()-100);
  present(display,face.drawEyes(display,200,bg),Face::kEyesTop,Face::kEyesHeight);
  assert(allocations==before);
  puts("Face: all expressions/attention poses, clipped bands, caption preservation, held mouth, input bounds and allocation-free drawing passed");
}
