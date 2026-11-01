#include "MuseReplyMailbox.h"
#include <cassert>
#include <string>
#include <thread>
#include <cstdio>
int main() {
  muse::ReplyMailbox replies;
  assert(replies.empty());
  assert(replies.push("first"));
  const auto *active=replies.peek();
  const auto generation=active->generation;
  assert(!replies.empty()); // DONE cannot resume capture while playback owns a slot.
  assert(replies.push("second"));
  assert(replies.push("third"));
  assert(replies.push("fourth"));
  assert(!replies.push("overflow"));
  replies.cancel();
  assert(replies.cancelled(generation));
  assert(std::string(active->text)=="first");
  assert(!replies.empty()); // cancellation waits for HTTP/I2S cleanup
  for(int i=0;i<4;++i) { assert(replies.cancelled(replies.peek()->generation)); replies.release(); }
  assert(replies.empty());
  assert(replies.push(std::string(4000,'x').c_str()));
  assert(!replies.cancelled(replies.peek()->generation));
  assert(strlen(replies.peek()->text)==1023);
  replies.release();
  const std::string utf8=std::string(1022,'x')+"é";
  assert(replies.push(utf8.c_str()));
  assert(strlen(replies.peek()->text)==1022);
  replies.release();
  std::thread consumer([&] {
    for(int i=0;i<50000;++i) {
      const muse::ReplyMailbox::Reply *reply;
      while(!(reply=replies.peek())) std::this_thread::yield();
      assert(std::string(reply->text)==std::to_string(i));
      replies.release();
    }
  });
  for(int i=0;i<50000;++i) {
    auto text=std::to_string(i);
    while(!replies.push(text.c_str())) std::this_thread::yield();
  }
  consumer.join();
  assert(replies.empty());
  puts("Muse reply ownership, cancellation, overflow and concurrent handoff passed");
}
