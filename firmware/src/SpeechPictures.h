#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "TtsStream.h"
namespace audio {
// Pictures wait here between the network and their sentence: the fetch runs
// seconds ahead of playback, so a GIF can arrive long before its cue fires.
// Decoding and drawing take the display, so the audio task only marks a slot
// ready and the main loop picks it up. Bytes live in caller-owned PSRAM; this
// object does not, since PSRAM can't be trusted with atomic read-modify-write.
class SpeechPictures {
 public:
  static constexpr int kSlots=4;
  void attach(uint8_t *storage) {storage_=storage;}
  // Fetch task. Null when every slot is taken; the picture is then skipped.
  uint8_t *reserve() {
    for(int slot=0;slot<kSlots;++slot) {
      if(state_[slot].load()==State::Free) {
        state_[slot].store(State::Filling);filling_=slot;
        return storage_+size_t(slot)*kPictureMaxBytes;
      }
    }
    return nullptr;
  }
  int8_t commit(size_t bytes) {
    bytes_[filling_]=bytes;state_[filling_].store(State::Held);
    return int8_t(filling_);
  }
  // Audio task. A newer picture replaces one the main loop hasn't reached.
  void fire(int8_t slot) {
    auto held=State::Held;
    if(slot<0 || slot>=kSlots || !state_[slot].compare_exchange_strong(held,State::Ready))return;
    free(ready_.exchange(slot));
  }
  // Main task, paired with release() once the bytes have been copied.
  bool take(const uint8_t *&data,size_t &bytes) {
    const int8_t slot=ready_.exchange(-1);
    auto ready=State::Ready;
    if(slot<0 || !state_[slot].compare_exchange_strong(ready,State::Showing))return false;
    showing_=slot;data=storage_+size_t(slot)*kPictureMaxBytes;bytes=bytes_[slot];
    return true;
  }
  void release() {
    if(showing_>=0)state_[showing_].store(State::Free);
    showing_=-1;
  }
  // Fetch task, once playback has stopped: pictures whose cue never fired.
  void clear() {
    free(ready_.exchange(-1));
    for(auto &state:state_) {auto held=State::Held;state.compare_exchange_strong(held,State::Free);}
  }
 private:
  enum class State:uint8_t {Free,Filling,Held,Ready,Showing};
  void free(int8_t slot) {
    auto ready=State::Ready;
    if(slot>=0)state_[slot].compare_exchange_strong(ready,State::Free);
  }
  uint8_t *storage_=nullptr;
  std::atomic<State> state_[kSlots]{};
  std::atomic<int8_t> ready_{-1};
  size_t bytes_[kSlots]{};
  int filling_=0;
  int8_t showing_=-1;
};
}
