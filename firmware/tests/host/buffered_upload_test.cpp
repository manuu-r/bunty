#include "BufferedUpload.h"
#include <cassert>
#include <vector>
#include <iostream>
struct Ops {
  uint32_t time=0,delay=500;size_t calls=0;
  bool cancel=false,online=true,error=false,began=false,ended=false;
  int failAt=-1,cancelAt=-1;
  std::vector<int16_t> sent;
  uint32_t now(){return time;}
  bool cancelled(){return cancel;}
  bool connected(){return online;}
  void begin(){began=true;}
  void audio(const int16_t *p,size_t n){
    assert(began && !ended && n<=1536);sent.insert(sent.end(),p,p+n);++calls;time+=delay;
    if(int(calls)==failAt)error=true;
    if(int(calls)==cancelAt)cancel=true;
  }
  bool failed(){return error;}
  void yield(){}
  void end(){ended=true;}
};
int main(){
  std::vector<int16_t> storage(muse::kUtteranceFrames),samples(16000*3+5120);
  for(size_t i=0;i<samples.size();++i)samples[i]=int16_t(i);
  muse::UtteranceBuffer note(storage.data());
  for(size_t i=0;i<samples.size();i+=320)
    assert(note.append(samples.data()+i,std::min(size_t(320),samples.size()-i)));
  Ops slow;assert(muse::uploadBuffered(note,slow)==muse::UploadResult::Sent);
  assert(slow.ended && slow.sent==samples && slow.time>1000); // slow queue preserves every sample
  Ops cancelled;cancelled.cancelAt=3;
  assert(muse::uploadBuffered(note,cancelled)==muse::UploadResult::Cancelled);
  assert(!cancelled.ended && cancelled.calls==3);
  Ops failed;failed.failAt=2;
  assert(muse::uploadBuffered(note,failed)==muse::UploadResult::Failed);
  assert(!failed.ended && failed.calls==2);
  Ops timeout;timeout.delay=5000;
  assert(muse::uploadBuffered(note,timeout)==muse::UploadResult::Timeout);
  assert(!timeout.ended && timeout.time==60000);
  Ops wrapped;wrapped.time=UINT32_MAX-5000;
  assert(muse::uploadBuffered(note,wrapped)==muse::UploadResult::Sent && wrapped.sent==samples);
  Ops offline;offline.online=false;
  assert(muse::uploadBuffered(note,offline)==muse::UploadResult::Disconnected && !offline.began);
  note.reset();assert(note.frames()==0);
  assert(!note.append(samples.data(),muse::kUtteranceFrames+1));
  assert(note.frames()==0 && !note.append(nullptr,1));
  std::vector<int16_t> full(muse::kUtteranceFrames);
  assert(note.append(full.data(),muse::kUtteranceFrames));
  assert(!note.append(samples.data(),1) && note.frames()==muse::kUtteranceFrames);
  note.reset();Ops empty;
  assert(muse::uploadBuffered(note,empty)==muse::UploadResult::Empty && !empty.began);
  std::cout<<"Buffered voice: capture bounds, lossless slow upload, cancellation, SDK failure, deadline and clock wrap passed\n";
}
