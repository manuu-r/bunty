#pragma once
#include "UtteranceBuffer.h"
#include <algorithm>
namespace muse {
enum class UploadResult {Sent,Empty,Cancelled,Disconnected,Timeout,Failed};
// Ops owns the SDK. Capture is already stopped; payload remains immutable until
// this returns. Queue waits never stall VAD or grow the recording allocation.
template<class Ops> UploadResult uploadBuffered(const UtteranceBuffer &note,Ops &ops) {
  if(!note.frames())return UploadResult::Empty;
  const uint32_t started=ops.now();
  auto check=[&](){
    if(ops.cancelled())return UploadResult::Cancelled;
    if(!ops.connected())return UploadResult::Disconnected;
    if(ops.now()-started>=60000)return UploadResult::Timeout;
    return UploadResult::Sent;
  };
  auto state=check();if(state!=UploadResult::Sent)return state;
  ops.begin();if(ops.failed())return UploadResult::Failed;
  for(size_t offset=0;offset<note.frames();) {
    state=check();if(state!=UploadResult::Sent)return state;
    const size_t count=std::min(size_t(1536),note.frames()-offset);
    ops.audio(note.data()+offset,count);offset+=count;
    if(ops.failed())return UploadResult::Failed;
    ops.yield();
  }
  state=check();if(state!=UploadResult::Sent)return state;
  ops.end();return UploadResult::Sent;
}
}
