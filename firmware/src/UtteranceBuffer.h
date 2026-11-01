#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace muse {
// 32.768 seconds at 16 kHz: covers the 30-second VAD cap plus pre-roll.
constexpr size_t kUtteranceFrames=524288;
class UtteranceBuffer {
 public:
  explicit UtteranceBuffer(int16_t *storage):storage_(storage){}
  void reset(){used_=0;}
  bool append(const int16_t *pcm,size_t frames){
    if(!storage_ || !pcm || frames>kUtteranceFrames-used_)return false;
    memcpy(storage_+used_,pcm,frames*sizeof(int16_t));used_+=frames;return true;
  }
  void dropTail(size_t frames){used_-=frames<used_?frames:used_;}
  size_t frames() const {return used_;}
  const int16_t *data() const {return storage_;}
 private:
  int16_t *storage_;size_t used_=0;
};
}
