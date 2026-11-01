#pragma once
#include <AnimatedGIF.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace muse {
constexpr size_t kGifMaxBytes=1024*1024;
constexpr unsigned kGifMaxFrames=512;
constexpr unsigned kGifMaxFps=10;
// Validate all block/rectangle boundaries before admitting untrusted animation.
inline bool validateGif(const uint8_t *p,size_t n,unsigned &frames,int *repeats=nullptr) {
  frames=0;if(repeats)*repeats=-1;
  if(!p || n<14 || n>kGifMaxBytes || (memcmp(p,"GIF87a",6)&&memcmp(p,"GIF89a",6)))return false;
  auto word=[](const uint8_t *b){return unsigned(b[0])|(unsigned(b[1])<<8);};
  const unsigned w=word(p+6),h=word(p+8);
  if(!w||!h||w>240||h>320)return false;
  size_t at=13;
  auto skip=[&](size_t size){if(size>n-at)return false;at+=size;return true;};
  if((p[10]&128) && !skip(size_t(3)<<(1+(p[10]&7))))return false;
  auto blocks=[&](){while(at<n){const unsigned count=p[at++];if(!count)return true;if(!skip(count))return false;}return false;};
  while(at<n) {
    const unsigned type=p[at++];
    if(type==0x3b)return frames>0; // trailer ends the GIF; bounded suffix is ignored
    if(type==0x21) {
      if(at==n)return false;
      const unsigned label=p[at++];
      if(label==0xff && repeats && n-at>=17 && p[at]==11 && p[at+12]==3 && p[at+13]==1 &&
         (!memcmp(p+at+1,"NETSCAPE2.0",11)||!memcmp(p+at+1,"ANIMEXTS1.0",11)))
        *repeats=int(word(p+at+14));
      if(label==0x01)return false; // plain text rendering is not supported
      if(label==0xf9) {
        if(n-at<6 || p[at]!=4 || p[at+5]!=0 || ((p[at+1]>>2)&7)>3)return false;
        at+=6;
      } else if(!blocks())return false;
    } else if(type==0x2c) {
      if(n-at<9 || ++frames>kGifMaxFrames)return false;
      const unsigned x=word(p+at),y=word(p+at+2),fw=word(p+at+4),fh=word(p+at+6),packed=p[at+8];at+=9;
      if(!fw||!fh||x>=w||y>=h||fw>w-x||fh>h-y)return false;
      if((packed&128) && !skip(size_t(3)<<(1+(packed&7))))return false;
      if(at==n || p[at]<2 || p[at]>8)return false;
      ++at;if(!blocks())return false;
    } else return false;
  }
  return false;
}
// Object, compressed file and two RGB565 canvases are owned in PSRAM by caller.
class GifCanvas {
 public:
  bool open(uint8_t *data,size_t bytes,uint16_t *canvas,uint16_t *restore,int row,uint16_t fill) {
    if(!canvas||!restore||!validateGif(data,bytes,frames_,&repeats_))return false;
    decoder_.begin();if(!decoder_.open(data,int(bytes),draw))return false;
    width_=decoder_.getCanvasWidth();height_=decoder_.getCanvasHeight();
    x0_=(240-width_)/2;y0_=row<0?(320-height_)/2:row;
    if(y0_<0 || y0_+height_>320)return false;
    zoom_=row<0;
    // Precompute pixel-centred nearest-neighbour sampling once per file.
    // Cover the panel while preserving aspect ratio, cropping about the centre.
    const bool widthScale=240*height_>=320*width_;
    for(int x=0;x<240;++x) {
      const int source=widthScale?((2*x+1)*width_)/480:
        ((2*x+1-240)*height_+320*width_)/640;
      screenX_[x]=uint16_t(x0_+std::max(0,std::min(width_-1,source)));
    }
    for(int y=0;y<320;++y) {
      const int source=widthScale?((2*y+1-320)*width_+240*height_)/480:
        ((2*y+1)*height_)/640;
      screenY_[y]=uint16_t(y0_+std::max(0,std::min(height_-1,source)));
    }
    canvas_=canvas;restore_=restore;fill_=fill;background_=fill;
    if((data[10]&128) && data[11]<(2u<<(data[10]&7))) {
      const uint8_t *c=data+13+data[11]*3;
      background_=uint16_t((c[0]>>3)<<11|(c[1]>>2)<<5|(c[2]>>3));
    }
    std::fill(canvas_,canvas_+240*320,fill_);
    previous_=0;index_=0;loops_=0;running_=true;failed_=false;return true;
  }
  bool next(uint32_t now) {
    if(!running_)return false;
    if(index_==frames_) {
      const int repeats=repeats_;
      if(repeats<0 || (repeats>0 && loops_>=unsigned(repeats))) {running_=false;return false;}
      ++loops_;decoder_.reset();index_=0;previous_=0;
      std::fill(canvas_,canvas_+240*320,fill_);
    }
    if(previous_==2)for(int y=py_;y<py_+ph_;++y)
      std::fill(canvas_+y*240+px_,canvas_+y*240+px_+pw_,previousBackground_);
    else if(previous_==3)memcpy(canvas_,restore_,240*320*2);
    started_=false;bad_=false;int delay=0;
    const int result=decoder_.playFrame(false,&delay,this);
    if(result<0 || decoder_.getLastError()!=GIF_SUCCESS || !started_ || bad_) {running_=false;failed_=true;return false;}
    ++index_;delay_=unsigned(std::max(100,std::min(delay,10000)));due_=now+delay_;return true;
  }
  bool due(uint32_t now) const {return running_ && int32_t(now-due_)>=0;}
  void displayed(uint32_t now) {due_=now+delay_;}
  bool running() const {return running_;}
  bool failed() const {return failed_;}
  const uint16_t *row(int y) const {return canvas_+y*240;}
  // Scale after disposal/transparency composition, using only one output row.
  void screenRow(int y,uint16_t *out) const {
    if(!zoom_) {memcpy(out,row(y),240*sizeof(uint16_t));return;}
    const uint16_t *source=row(screenY_[y]);
    for(int x=0;x<240;++x)out[x]=source[screenX_[x]];
  }
  bool fullscreen() const {return zoom_;}
  int width() const {return width_;} int height() const {return height_;}
 private:
  static void draw(GIFDRAW *d) {
    auto &g=*static_cast<GifCanvas *>(d->pUser);
    if(d->iX<0||d->iY<0||d->iWidth<=0||d->iHeight<=0||d->iX+d->iWidth>g.width_||
       d->iY+d->iHeight>g.height_||d->y<0||d->y>=d->iHeight) {g.bad_=true;return;}
    if(!g.started_) {
      g.started_=true;
      if(d->ucDisposalMethod==3)memcpy(g.restore_,g.canvas_,240*320*2);
      g.px_=g.x0_+d->iX;g.py_=g.y0_+d->iY;g.pw_=d->iWidth;g.ph_=d->iHeight;
      g.previous_=d->ucDisposalMethod;
      g.previousBackground_=(d->ucHasTransparency && d->ucTransparent==d->ucBackground)?g.fill_:g.background_;
    }
    auto *line=g.canvas_+(g.y0_+d->iY+d->y)*240+g.x0_+d->iX;
    for(int x=0;x<d->iWidth;++x)if(!d->ucHasTransparency||d->pPixels[x]!=d->ucTransparent)
      line[x]=d->pPalette[d->pPixels[x]];
  }
  AnimatedGIF decoder_;
  uint16_t *canvas_=nullptr,*restore_=nullptr,fill_=0,background_=0,previousBackground_=0;
  uint16_t screenX_[240]={},screenY_[320]={};
  bool zoom_=false;
  int repeats_=-1;
  int width_=0,height_=0,x0_=0,y0_=0,px_=0,py_=0,pw_=0,ph_=0;
  unsigned frames_=0,index_=0,loops_=0,previous_=0;
  uint32_t due_=0,delay_=100;bool started_=false,bad_=false,running_=false,failed_=false;
};
}
