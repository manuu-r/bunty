#include "MuseVad.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

using Event = muse::Vad::Event;
static Event frame(muse::Vad &vad, int16_t amplitude) {
  std::array<int16_t, 320> pcm{};
  for (size_t i=0; i<pcm.size(); ++i) pcm[i]=i%2 ? amplitude : -amplitude;
  return vad.process(pcm.data(),pcm.size());
}
static void start(muse::Vad &vad) {
  for (int i=0;i<4;++i) assert(frame(vad,2000)==Event::None);
  assert(frame(vad,2000)==Event::Started);
}
int main() {
  muse::Vad vad;
  for (int i=0;i<400;++i) assert(frame(vad,100)==Event::None);
  // A knock is not a turn; its onset must not leak into later speech.
  assert(frame(vad,10000)==Event::None);
  assert(frame(vad,100)==Event::None);
  start(vad);
  for (int i=0;i<30;++i) assert(frame(vad,2000)==Event::None);
  // A short hesitation survives; more speech resets the silence timer.
  for (int i=0;i<30;++i) assert(frame(vad,100)==Event::None);
  assert(frame(vad,2000)==Event::None);
  for (int i=0;i<44;++i) assert(frame(vad,100)==Event::None);
  assert(frame(vad,100)==Event::Ended);
  // The 900 ms endpoint is silence nobody needs to upload; 250 ms stays.
  assert(vad.excessQuiet()==14400-muse::Vad::kTailFrames);
  vad.reset(); assert(vad.excessQuiet()==0); start(vad);
  for (int i=0;i<1494;++i) assert(frame(vad,2000)==Event::None);
  assert(frame(vad,2000)==Event::Ended);  // 30-second safety cap.
  assert(vad.excessQuiet()==0);           // cut off mid-speech: keep every word.
  vad.reset();
  assert(vad.process(nullptr,0)==Event::None);
  assert(!vad.speaking());
  // Same 100 ms onset with 10 ms chunks instead of 20 ms chunks.
  std::array<int16_t,160> small{}; small.fill(2000);
  for(int i=0;i<9;++i) assert(vad.process(small.data(),small.size())==Event::None);
  assert(vad.process(small.data(),small.size())==Event::Started);

  std::array<int16_t,muse::Vad::kPreRollFrames> storage{};
  muse::PreRoll ring(storage.data());
  std::vector<int16_t> input(8000), output;
  for(size_t i=0;i<input.size();++i) input[i]=static_cast<int16_t>(i);
  ring.push(input.data(),input.size());
  ring.flush([&](const int16_t *p,size_t n) { output.insert(output.end(),p,p+n); });
  assert(output.size()==storage.size());
  for(size_t i=0;i<output.size();++i) assert(output[i]==input[input.size()-output.size()+i]);
  // Short notes and reset must not reuse samples from the previous turn.
  output.clear(); ring.push(input.data(),17); ring.reset(); ring.push(input.data()+50,23);
  ring.flush([&](const int16_t *p,size_t n) { output.insert(output.end(),p,p+n); });
  assert(output.size()==23 && output.front()==50 && output.back()==72);
  output.clear(); ring.flush([&](const int16_t *p,size_t n) { output.insert(output.end(),p,p+n); });
  assert(output.empty());
  std::puts("Muse VAD and pre-roll checks passed");
}
