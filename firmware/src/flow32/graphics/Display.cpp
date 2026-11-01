#include "Log.h"
#include "flow32/graphics/Display.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

namespace {

/** Blend `fg` over `bg` at 4-bit coverage (0 = transparent, 15 = opaque). */
uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t cover4) {
  if (cover4 >= 15) return fg;
  if (cover4 == 0) return bg;
  const uint16_t inverse = 15 - cover4;
  const uint16_t r = ((fg >> 11) & 0x1f) * cover4 + ((bg >> 11) & 0x1f) * inverse;
  const uint16_t g = ((fg >> 5) & 0x3f) * cover4 + ((bg >> 5) & 0x3f) * inverse;
  const uint16_t b = (fg & 0x1f) * cover4 + (bg & 0x1f) * inverse;
  return static_cast<uint16_t>(((r / 15) << 11) | ((g / 15) << 5) | (b / 15));
}

/** Coverage from a signed distance: <= -0.5 opaque, >= +0.5 transparent. */
uint8_t coverFromSd(float sd) {
  const float c = 0.5f - sd;
  if (c <= 0.f) return 0;
  if (c >= 1.f) return 15;
  return static_cast<uint8_t>(c * 15.f + 0.5f);
}

int16_t clampRadius(int16_t w, int16_t h, int16_t r) {
  if (r < 0) r = 0;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  return r;
}

} // namespace

Display::Display(const DisplayPanel &panel, DisplayTransport &transport)
    : Adafruit_GFX(panel.width, panel.height), panel_(panel),
      transport_(transport) {}

Display::~Display() { end(); }

size_t Display::bufferBytes() const {
  if (panel_.width <= 0 || panel_.height <= 0) return 0;
  return static_cast<size_t>(panel_.width) * panel_.height * sizeof(uint16_t);
}

bool Display::begin() {
  if (ready()) return true;
  const size_t bytes = bufferBytes();
  if (bytes == 0 || panel_.width > 240 || panel_.height > 320) return false;
  if (!transport_.begin(panel_)) return false;
  transportBegun_ = true;
  setBacklight(true);

  if (panel_.preferPsram) {
    fb_ = static_cast<uint16_t *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (!fb_) mono_ = static_cast<uint8_t *>(heap_caps_calloc((bytes / 2 + 7) / 8, 1,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  line_ = static_cast<uint16_t *>(heap_caps_malloc(panel_.width * 2 * kTransferRows,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  if ((!fb_ && !mono_) || !line_) {
    end();
    return false;
  }
  if (fb_) memset(fb_, 0, bytes);
  else Serial.println("[display] color PSRAM unavailable; using internal monochrome framebuffer");
  memset(dirty_, 1, sizeof(dirty_));

  setTextColor(0xFFFF);
  setTextWrap(false);
  return true;
}

void Display::end() {
  heap_caps_free(mono_);
  heap_caps_free(line_);
  mono_ = nullptr;
  line_ = nullptr;
  auraOpacity_=0;auraColor_=0;auraPending_=false;
  if (fb_ || transportBegun_) setBacklight(false);
  if (fb_) {
    heap_caps_free(fb_);
    fb_ = nullptr;
  }
  if (transportBegun_) {
    transport_.end();
    transportBegun_ = false;
  }
}

void Display::setBacklight(bool on) { transport_.setBacklight(on); }

void Display::clear(uint16_t color) {
  fillRect(0, 0, panel_.width, panel_.height, color);
}

void Display::present() { present(0, 0, panel_.width, panel_.height); }

void Display::present(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (!ready() || w <= 0 || h <= 0) return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > panel_.width) w = panel_.width - x;
  if (y + h > panel_.height) h = panel_.height - y;
  if (w <= 0 || h <= 0) return;

  const int columns = (panel_.width + 15) / 16;
  for (int ty = y / 16; ty <= (y + h - 1) / 16; ++ty) {
    for (int tx = x / 16; tx <= (x + w - 1) / 16; ++tx) {
      if (!dirty_[ty * columns + tx]) continue;
      const int first = tx;
      while (tx < (x + w - 1) / 16 && dirty_[ty * columns + tx + 1]) ++tx;
      const int left = max(int(x), first * 16);
      const int top = max(int(y), ty * 16);
      const int right = min(int(x + w), min(int(panel_.width), tx * 16 + 16));
      const int bottom = min(int(y + h), min(int(panel_.height), ty * 16 + 16));
      if(!transferRegion(left,top,right-left,bottom-top))return;
      for (int tile = first; tile <= tx; ++tile) {
        if (left <= tile * 16 && top == ty * 16 &&
            right >= min(int(panel_.width), tile * 16 + 16) &&
            bottom == min(int(panel_.height), ty * 16 + 16)) dirty_[ty * columns + tile] = 0;
      }
    }
  }
}

uint16_t Display::auraPixel(int16_t x,int16_t y,uint16_t base) const {
  if(!auraOpacity_)return base;
  const int edge=min(min(int(x),int(y)),min(int(panel_.width-1-x),int(panel_.height-1-y)));
  if(edge>=3)return base;
  constexpr uint8_t falloff[]={15,9,3};
  return blend565(auraColor_,base,uint8_t(auraOpacity_*falloff[edge]/15));
}
bool Display::transferRegion(int16_t x,int16_t y,int16_t w,int16_t h) {
  if(w<=0 || h<=0)return true;
  // The buffer holds eight full-width rows. Narrow edge strips can pack
  // more rows into the same 3,840 bytes, reducing aura refresh to four submits.
  const int rowsPerTransfer=panel_.width*kTransferRows/w;
  for(int row=y;row<y+h;row+=rowsPerTransfer) {
    const int rows=min(rowsPerTransfer,int(y+h-row));
    for(int n=0;n<rows;++n) {
      if(fb_)memcpy(line_+n*w,fb_+(row+n)*panel_.width+x,w*sizeof(uint16_t));
      else for(int col=0;col<w;++col)line_[n*w+col]=pixel(x+col,row+n);
      if(auraOpacity_)for(int col=0;col<w;++col)
        line_[n*w+col]=auraPixel(x+col,row+n,line_[n*w+col]);
    }
    if(!transport_.present(line_,x,row,w,rows,w))return false;
    transferredPixels_+=w*rows;++transfers_;
  }
  return true;
}
void Display::setAura(uint16_t color,uint8_t opacity) {
  if(!ready())return;
  opacity=min(uint8_t(15),opacity);
  if(color!=auraColor_ || opacity!=auraOpacity_) {
    auraColor_=color;auraOpacity_=opacity;auraPending_=true;
  }
  if(!auraPending_)return;
  const int w=panel_.width,h=panel_.height;
  const int edge=min(3,min(w/2,h/2));
  if(edge<=0) {auraPending_=false;return;}
  // These submits do not consume dirty tiles: pending content still gets its
  // normal present(). Every normal submit also composes the current aura.
  if(!transferRegion(0,0,w,edge) || !transferRegion(0,h-edge,w,edge) ||
     !transferRegion(0,edge,edge,h-2*edge) || !transferRegion(w-edge,edge,edge,h-2*edge))return;
  auraPending_=false;
}

uint16_t Display::pixel(int16_t x, int16_t y) const {
  const size_t index = size_t(y) * panel_.width + x;
  return fb_ ? fb_[index] : (mono_[index / 8] & (1 << (index % 8)) ? 0xffff : 0);
}
void Display::markDirty(int16_t x, int16_t y) {
  dirty_[(y / 16) * ((panel_.width + 15) / 16) + x / 16] = 1;
}
void Display::reportMemory() const {
  unsigned dirty = 0;
  for (uint8_t tile : dirty_) dirty += tile != 0;
  logPrintf("[memory] fb color_psram=%u mono_internal=%u line_dma=%u dirty=%u pixels=%lu transfers=%lu\n",
      fb_ ? unsigned(bufferBytes()) : 0, mono_ ? unsigned((bufferBytes() / 2 + 7) / 8) : 0,
      line_ ? unsigned(panel_.width * 2 * kTransferRows) : 0, dirty,
      (unsigned long)transferredPixels_, (unsigned long)transfers_);
}

void Display::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if ((!fb_ && !mono_) || x < 0 || y < 0 || x >= panel_.width || y >= panel_.height) return;
  const size_t index = size_t(y) * panel_.width + x;
  if (fb_) {
    if (fb_[index] != color) { fb_[index] = color; markDirty(x,y); }
    return;
  }
  const bool lit = (((color >> 11) & 31) * 8 + ((color >> 5) & 63) * 4 + (color & 31) * 8) >= 192;
  if (pixel(x,y) != (lit ? 0xffff : 0)) markDirty(x,y);
  if (lit) mono_[index / 8] |= 1 << (index % 8);
  else mono_[index / 8] &= ~(1 << (index % 8));
}

void Display::blendPixel(int16_t x, int16_t y, uint16_t fg, uint8_t cover4) {
  if ((!fb_ && !mono_) || cover4 == 0) return;
  if (x < 0 || y < 0 || x >= panel_.width || y >= panel_.height) return;
  drawPixel(x, y, blend565(fg, pixel(x, y), cover4));
}

void Display::writeRow(int16_t x, int16_t y, const uint16_t *colors, int16_t count) {
  if ((!fb_ && !mono_) || !colors || y < 0 || y >= panel_.height) return;
  if (x < 0) {
    colors -= x;
    count += x;
    x = 0;
  }
  if (x + count > panel_.width) count = panel_.width - x;
  if (count <= 0) return;
  const size_t start = size_t(y) * panel_.width + x;
  if (fb_) {
    const size_t bytes=size_t(count)*sizeof(uint16_t);
    if (!memcmp(fb_+start,colors,bytes)) return;
    memcpy(fb_+start,colors,bytes);
  }
  if (mono_) for (int16_t i = 0; i < count; ++i) {
    const uint16_t color = colors[i];
    const size_t index = start + i;
    const bool lit = (((color >> 11) & 31) * 8 + ((color >> 5) & 63) * 4 + (color & 31) * 8) >= 192;
    if (lit) mono_[index / 8] |= 1 << (index % 8);
    else mono_[index / 8] &= ~(1 << (index % 8));
  }
  for (int16_t tile = x / 16; tile <= (x + count - 1) / 16; ++tile) markDirty(tile * 16, y);
}

void Display::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  if ((!fb_ && !mono_) || y < 0 || y >= panel_.height) return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (x + w > panel_.width) w = panel_.width - x;
  if (w <= 0) return;
  for (int16_t i = 0; i < w; ++i) drawPixel(x + i, y, color);
}

void Display::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  if ((!fb_ && !mono_) || x < 0 || x >= panel_.width) return;
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (y + h > panel_.height) h = panel_.height - y;
  if (h <= 0) return;
  for (int16_t i = 0; i < h; ++i) {
    drawPixel(x, y + i, color);
  }
}

void Display::fillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                       uint16_t color) {
  for (int16_t row = 0; row < h; ++row) {
    drawFastHLine(x, y + row, w, color);
  }
}

void Display::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                            int16_t r, uint16_t color) {
  if ((!fb_ && !mono_) || w <= 0 || h <= 0) return;
  r = clampRadius(w, h, r);
  if (r <= 0) {
    fillRect(x, y, w, h, color);
    return;
  }

  // Solid body -- anti-aliasing is only needed in the four corner quarters.
  if (h > 2 * r) {
    fillRect(x, static_cast<int16_t>(y + r), w, static_cast<int16_t>(h - 2 * r),
             color);
  }
  if (w > 2 * r) {
    fillRect(static_cast<int16_t>(x + r), y, static_cast<int16_t>(w - 2 * r), r,
             color);
    fillRect(static_cast<int16_t>(x + r), static_cast<int16_t>(y + h - r),
             static_cast<int16_t>(w - 2 * r), r, color);
  }

  const float rf = static_cast<float>(r);
  const auto paintCorner = [&](int16_t x0, int16_t y0, float cx, float cy) {
    for (int16_t j = 0; j < r; ++j) {
      for (int16_t i = 0; i < r; ++i) {
        const float dx = (x0 + i + 0.5f) - cx;
        const float dy = (y0 + j + 0.5f) - cy;
        const uint8_t cover = coverFromSd(sqrtf(dx * dx + dy * dy) - rf);
        if (cover == 0) continue;
        blendPixel(static_cast<int16_t>(x0 + i), static_cast<int16_t>(y0 + j),
                   color, cover);
      }
    }
  };

  paintCorner(x, y, x + rf, y + rf);
  paintCorner(static_cast<int16_t>(x + w - r), y, x + w - rf, y + rf);
  paintCorner(x, static_cast<int16_t>(y + h - r), x + rf, y + h - rf);
  paintCorner(static_cast<int16_t>(x + w - r), static_cast<int16_t>(y + h - r),
              x + w - rf, y + h - rf);
}
