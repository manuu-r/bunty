#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

namespace muse {
// One producer (Muse event task), one consumer (TTS task). The consumer retains
// its slot until HTTP and speaker cleanup finish. Cancellation never frees a
// slot underneath playback; it invalidates jobs and waits for their release.
class ReplyMailbox {
 public:
  struct Reply { uint32_t generation; char text[1024]; };
  bool push(const char *text) {
    const auto head=head_.load();
    if(head-tail_.load()>=kCapacity) return false;
    auto &reply=slots_[head%kCapacity];
    reply.generation=generation_.load();
    auto length=strnlen(text,sizeof(reply.text)-1);
    while(length && (static_cast<unsigned char>(text[length])&0xc0)==0x80) --length;
    memcpy(reply.text,text,length); reply.text[length]=0;
    head_.store(head+1);
    return true;
  }
  const Reply *peek() const {
    const auto tail=tail_.load();
    return tail==head_.load()?nullptr:&slots_[tail%kCapacity];
  }
  // Each index has one writer, and PSRAM can't be trusted with atomic read-modify-write.
  void release() { tail_.store(tail_.load()+1); }
  void cancel() { generation_.store(generation_.load()+1); }
  bool cancelled(uint32_t generation) const { return generation!=generation_.load(); }
  bool empty() const { return head_.load()==tail_.load(); }
 private:
  // A power of two keeps index wrapping correct across uint32_t overflow.
  static constexpr uint32_t kCapacity=4;
  Reply slots_[kCapacity]{};
  std::atomic<uint32_t> head_{0},tail_{0},generation_{0};
};
}
