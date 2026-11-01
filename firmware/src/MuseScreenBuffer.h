#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace muse {
// Caller serializes access and supplies PSRAM. The last screen is kept while
// downloading; cancelled or failed transfers never commit their partial pixels.
class ScreenBuffer {
 public:
  void bind(uint16_t *pixels,int width,int height) { pixels_=pixels; width_=width; height_=height; }
  bool begin(uint16_t background) {
    if(!pixels_ || fetching_) return false;
    fetching_=true; cancelled_=false;
    std::fill(pixels_,pixels_+size_t(width_)*height_,background);
    return true;
  }
  bool fetching() const { return fetching_; }
  bool current() const { return fetching_ && !cancelled_; }
  void cancel() { if(fetching_) cancelled_=true; }
  bool write(int x,int y,int w,int h,const uint8_t *bigEndian) {
    if(!current() || !bigEndian || x<0 || y<0 || w<=0 || h<=0 ||
       x>=width_ || y>=height_ || w>width_-x || h>height_-y) return false;
    for(int row=0;row<h;++row) for(int col=0;col<w;++col) {
      const size_t source=(size_t(row)*w+col)*2;
      pixels_[size_t(y+row)*width_+x+col]=uint16_t(uint16_t(bigEndian[source])<<8)|bigEndian[source+1];
    }
    return true;
  }
  bool finish(bool success) {
    const bool commit=current() && success;
    fetching_=false; cancelled_=false;
    return commit;
  }
  const uint16_t *row(int y) const { return pixels_+size_t(y)*width_; }
 private:
  uint16_t *pixels_=nullptr;
  int width_=0,height_=0;
  bool fetching_=false,cancelled_=false;
};
}  // namespace muse
