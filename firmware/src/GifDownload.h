#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace muse {
// Caller owns data even on failure. Grow only as bytes arrive; a short read
// is not EOF. Ops supplies a PSRAM-only allocator and a bounded/cancellable read.
template<class Ops>
const char *downloadGif(Ops &ops,uint8_t *&data,size_t &bytes,size_t limit) {
  bytes=0;size_t capacity=0;
  for(;;) {
    if(bytes==limit) {
      uint8_t extra;const int count=ops.read(&extra,1);
      if(count<0)return "GIF download failed";
      if(count)return "GIF exceeds 1 MiB";
      break;
    }
    if(bytes==capacity) {
      const size_t next=std::min(limit,capacity?capacity*2:size_t(65536));
      auto *grown=ops.resize(data,next);
      if(!grown)return "GIF memory unavailable";
      data=grown;capacity=next;
    }
    const size_t want=std::min(size_t(4096),capacity-bytes);
    const int count=ops.read(data+bytes,want);
    if(count<0 || size_t(count)>want)return "GIF download failed";
    if(!count)break;
    bytes+=size_t(count);
  }
  if(bytes && bytes<capacity) {
    // Shrinking is optional; allocation failure must retain the original block.
    if(auto *exact=ops.resize(data,bytes))data=exact;
  }
  return nullptr;
}
}
