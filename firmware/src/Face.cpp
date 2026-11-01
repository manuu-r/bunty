#include "Face.h"
#include <math.h>
#include <stddef.h>
#include "flow32/graphics/Display.h"
namespace {
using Eye=Face::Eye;using Mouth=Face::Mouth;using Rect=Face::Rect;
constexpr Eye kEyes[][2]={
  {{58,76,.72f,0,0,0,0},{58,76,.72f,0,0,0,0}},
  {{60,60,.85f,4,.30f,-.08f,0},{60,60,.85f,4,.30f,-.08f,0}},
  {{62,62,1,-2,0,0,1},{62,62,1,-2,0,0,1}},
  {{64,84,.82f,-4,0,0,0},{64,84,.82f,-4,0,0,0}},
  {{60,52,.48f,5,.24f,.42f,0},{60,52,.48f,5,.24f,.42f,0}},
  {{58,65,.85f,5,.25f,-.35f,0},{58,65,.85f,5,.25f,-.35f,0}},
  {{60,80,.75f,-5,0,0,0},{54,57,.85f,5,.18f,0,0}},
  {{58,65,.8f,3,.38f,0,0},{58,75,.8f,-3,0,0,0}}
};
constexpr Mouth kMouths[]={
  {34,6,15,0},{26,4,10,0},{42,9,16,0},{46,10,19,0},
  {34,-4,10,0},{28,-6,10,0},{20,0,16,0},{32,3,12,7}
};
static_assert(sizeof(kEyes)/sizeof(kEyes[0])==size_t(Emotion::Count),"eye per emotion");
static_assert(sizeof(kMouths)/sizeof(kMouths[0])==size_t(Emotion::Count),"mouth per emotion");
float clamp(float v,float a,float b){return fmaxf(a,fminf(b,v));}
float approach(uint32_t elapsed,float tau){return 1-expf(-float(elapsed)/tau);}
void blend(float &v,float goal,float k){v+=(goal-v)*k;}
float distance(float x,float y){return sqrtf(x*x+y*y);}
float coverage(float d){return clamp(.5f-d,0,1);}
constexpr uint16_t kIvory=uint16_t((233>>3)<<11|(241>>2)<<5|(223>>3));
constexpr uint16_t kMint=uint16_t((193>>3)<<11|(225>>2)<<5|(156>>3));
uint16_t mix(uint16_t a,uint16_t b,float t){
  const int ar=(a>>11)&31,ag=(a>>5)&63,ab=a&31;
  return uint16_t((int(ar+(((b>>11)&31)-ar)*t+.5f)<<11)|
    (int(ag+(((b>>5)&63)-ag)*t+.5f)<<5)|int(ab+((b&31)-ab)*t+.5f));
}
Rect unite(Rect a,Rect b){
  if(a.x1<=a.x0)return b;
  if(b.x1<=b.x0)return a;
  return {int16_t(fminf(a.x0,b.x0)),int16_t(fminf(a.y0,b.y0)),int16_t(fmaxf(a.x1,b.x1)),int16_t(fmaxf(a.y1,b.y1))};
}
Rect bounds(float x0,float y0,float x1,float y1,int top,int bottom){
  return {int16_t(clamp(floorf(x0),0,240)),int16_t(clamp(floorf(y0),top,bottom)),
          int16_t(clamp(ceilf(x1),0,240)),int16_t(clamp(ceilf(y1),top,bottom))};
}
struct EyeFrame {Eye eye;float cx,cy,hx,hy,radius,pupilX,pupilY;};
float shape(const EyeFrame &f,float x,float y){
  const float qx=fabsf(x)-f.hx+f.radius,qy=fabsf(y)-f.hy+f.radius;
  float d=(qx>0 && qy>0?distance(qx,qy):fmaxf(qx,qy))-f.radius;
  const float lid=-f.hy+f.eye.lid*f.eye.height+f.eye.slant*x;
  if(f.eye.lid>0 || fabsf(f.eye.slant)>.001f)d=fmaxf(d,lid-y);
  if(f.eye.arc>.001f){
    // A clean crescent with rounded ends, using the same eye envelope.
    const float radius=f.hx*.82f,centre=radius*.4f;
    float arc;
    if(y<=centre-radius*.42f)arc=fabsf(distance(x,y-centre)-radius)-4;
    else arc=distance(fabsf(x)-radius*.906f,y-(centre-radius*.42f))-4;
    d+=(arc-d)*f.eye.arc;
  }
  return d;
}
}
float Face::random(){seed_=seed_*1664525u+1013904223u;return (seed_>>8)/16777216.0f;}
void Face::reset(uint32_t now){
  const auto mood=static_cast<uint8_t>(emotion_);
  eyes_[0]=kEyes[mood][0];eyes_[1]=kEyes[mood][1];mouth_=kMouths[mood];
  gazeX_=gazeY_=targetX_=targetY_=0;mouthOpen_=0;lastMouthOpen_=-1;lastMouthMood_=255;
  lastAdvanceAt_=lastMouthAt_=now;nextBlinkAt_=now+1800;nextGlanceAt_=now+2500;blinking_=false;
  lastEyes_={0,kEyesTop,240,kEyesTop+kEyesHeight};
  lastMouth_={kMouthLeft,kMouthTop,kMouthLeft+kMouthWidth,kMouthTop+kMouthHeight};
  lastSignature_[0]=-1e6f;
}
void Face::advance(uint32_t now){
  const uint32_t elapsed=now-lastAdvanceAt_;lastAdvanceAt_=now;
  const auto mood=static_cast<uint8_t>(emotion_);
  const float k=approach(elapsed,140);
  for(int i=0;i<2;++i){
    Eye goal=kEyes[mood][i];
    if(attention_==Attention::Listening){goal.height+=5;goal.lid*=.4f;}
    if(attention_==Attention::Thinking || attention_==Attention::Working){goal.lid=fmaxf(goal.lid,i==0?.28f:.08f);goal.dy-=2;}
    auto &e=eyes_[i];
    blend(e.width,goal.width,k);blend(e.height,goal.height,k);blend(e.round,goal.round,k);
    blend(e.dy,goal.dy,k);blend(e.lid,goal.lid,k);blend(e.slant,goal.slant,k);blend(e.arc,goal.arc,k);
  }
  if(int32_t(now-nextGlanceAt_)>=0){
    targetX_=(random()-.5f)*8;targetY_=(random()-.5f)*4;
    nextGlanceAt_=now+3200+uint32_t(random()*3000);
  }
  const float gx=attention_==Attention::Working?8*sinf(float(now%62832)*.0024f):
    attention_==Attention::Thinking?8:attention_==Attention::Listening?0:targetX_;
  const float gy=attention_==Attention::Listening?-2:targetY_;
  blend(gazeX_,gx,approach(elapsed,85));blend(gazeY_,gy,approach(elapsed,85));
  if(!blinking_ && int32_t(now-nextBlinkAt_)>=0){blinking_=true;blinkAt_=now;}
}
Face::Rect Face::drawEyes(Display &display,uint32_t now,uint16_t background){
  advance(now);
  float blink=0;
  if(blinking_){
    const uint32_t elapsed=now-blinkAt_;
    if(elapsed>=240){blinking_=false;nextBlinkAt_=now+3400+uint32_t(random()*2800);}
    else if(elapsed<80)blink=float(elapsed)/80;
    else if(elapsed<120)blink=1;
    else blink=1-float(elapsed-120)/120;
  }
  EyeFrame frames[2];Rect drawn{};
  float signature[19]{};
  for(int i=0;i<2;++i){
    auto &f=frames[i];f.eye=eyes_[i];
    f.eye.height=fmaxf(6,f.eye.height*(1-.92f*blink));f.eye.lid*=1-blink;
    f.eye.slant*=1-blink;f.eye.arc*=1-blink;
    f.cx=i==0?72:168;f.cy=156+f.eye.dy;
    f.hx=f.eye.width/2;f.hy=f.eye.height/2;f.radius=f.eye.round*fminf(f.hx,f.hy);
    f.pupilX=gazeX_+(emotion_==Emotion::Sassy?-5:0);f.pupilY=gazeY_;
    const auto b=bounds(f.cx-f.hx-2,f.cy-f.hy-2,f.cx+f.hx+2,f.cy+f.hy+2,kEyesTop,kEyesTop+kEyesHeight);
    drawn=unite(drawn,b);
    const float s[]={f.cx,f.cy,f.hx,f.hy,f.eye.lid,f.eye.slant,f.eye.arc,f.radius};
    for(int j=0;j<8;++j)signature[i*8+j]=s[j];
  }
  signature[16]=gazeX_;signature[17]=gazeY_;signature[18]=float(emotion_);
  bool changed=false;
  for(size_t i=0;i<19;++i)if(fabsf(signature[i]-lastSignature_[i])>.06f)changed=true;
  if(!changed)return {};
  for(size_t i=0;i<19;++i)lastSignature_[i]=signature[i];
  const Rect area=unite(drawn,lastEyes_);lastEyes_=drawn;
  uint16_t row[240];
  for(int py=area.y0;py<area.y1;++py){
    for(int px=area.x0;px<area.x1;++px){
      uint16_t color=background;
      for(const auto &f:frames){
        const float x=px+.5f-f.cx,y=py+.5f-f.cy;
        if(fabsf(x)>f.hx+2 || fabsf(y)>f.hy+2)continue;
        const float fill=coverage(shape(f,x,y));
        if(fill<=0)continue;
        color=mix(background,kIvory,fill);
        // Dark pupils are clipped by the eyelid and vanish in closed crescents.
        if(f.eye.arc<.98f && blink<.95f){
          const float radius=emotion_==Emotion::Excited?10:12;
          const float pupil=coverage(distance(x-f.pupilX,y-f.pupilY)-radius)*(1-f.eye.arc)*fill;
          color=mix(color,background,pupil);
          const float glint=coverage(distance(x-f.pupilX+3.5f,y-f.pupilY+4)-1.8f)*pupil;
          color=mix(color,kMint,glint);
        }
      }
      row[px-area.x0]=color;
    }
    display.writeRow(area.x0,py,row,area.x1-area.x0);
  }
  return area;
}
Face::Rect Face::drawMouth(Display &display,float open,uint32_t now,uint16_t background,float opacity){
  if(!isfinite(opacity))opacity=0;
  opacity=clamp(opacity,0,1);
  const auto mood=static_cast<uint8_t>(emotion_);const auto &goal=kMouths[mood];
  const uint32_t elapsed=now-lastMouthAt_;lastMouthAt_=now;
  const float k=approach(elapsed,120);
  blend(mouth_.width,goal.width,k);blend(mouth_.curve,goal.curve,k);
  blend(mouth_.depth,goal.depth,k);blend(mouth_.asym,goal.asym,k);
  if(!isfinite(open))open=0;
  // Lips lead the shape: syllables are about 200 ms, so a slow mouth reads as a wobble.
  blend(mouthOpen_,clamp(open,0,1),approach(elapsed,open>mouthOpen_?28:70));
  const bool settled=fabsf(mouth_.width-goal.width)<.05f && fabsf(mouth_.curve-goal.curve)<.05f &&
    fabsf(mouth_.depth-goal.depth)<.05f && fabsf(mouth_.asym-goal.asym)<.05f;
  if(settled && mood==lastMouthMood_ && fabsf(mouthOpen_-lastMouthOpen_)<.02f && fabsf(opacity-lastMouthAlpha_)<.01f)return {};
  lastMouthOpen_=mouthOpen_;lastMouthMood_=mood;lastMouthAlpha_=opacity;
  const float half=mouth_.width/2,depth=mouth_.depth*mouthOpen_;
  const Rect drawn=bounds(120-half-3,230-fabsf(mouth_.curve)-5,120+half+3,230+depth+fabsf(mouth_.curve)+4,kMouthTop,kMouthTop+kMouthHeight);
  const Rect area=unite(drawn,lastMouth_);lastMouth_=drawn;
  uint16_t row[240];
  for(int py=area.y0;py<area.y1;++py){
    for(int px=area.x0;px<area.x1;++px){
      const float x=px+.5f-120,y=py+.5f-230;
      const float t=clamp(x/half,-1,1),bow=1-t*t;
      const float line=mouth_.curve*bow-mouth_.asym*t*.4f;
      float d=fmaxf(fabsf(y-line)-1.7f,fabsf(x)-half);
      if(depth>1){
        const float top=line-1.7f,low=line+depth*sqrtf(bow)+1.7f;
        d=fmaxf(fmaxf(top-y,y-low),fabsf(x)-half);
      }
      row[px-area.x0]=mix(background,kMint,coverage(d)*opacity);
    }
    display.writeRow(area.x0,py,row,area.x1-area.x0);
  }
  return area;
}
