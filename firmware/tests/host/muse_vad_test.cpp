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
  for (int i=0;i<32;++i) assert(frame(vad,100)==Event::None);
  assert(frame(vad,100)==Event::Ended);
  // The 660 ms endpoint is silence nobody needs to upload; 250 ms stays.
  assert(vad.excessQuiet()==muse::Vad::kPauseFrames-muse::Vad::kTailFrames);
  vad.reset(); assert(vad.excessQuiet()==0); start(vad);
  // Speech-like: the level swings word to word, so it is never mistaken for a fan.
  for (int i=0;i<1494;++i) assert(frame(vad,static_cast<int16_t>(1500+(i%9)*300))==Event::None);
  assert(frame(vad,2000)==Event::Ended);  // 30-second safety cap.
  assert(vad.excessQuiet()==0);           // cut off mid-speech: keep every word.
  vad.reset();
  assert(vad.process(nullptr,0)==Event::None);
  assert(!vad.speaking());
  // Same 100 ms onset with 10 ms chunks instead of 20 ms chunks.
  std::array<int16_t,160> small{}; small.fill(2000);
  for(int i=0;i<9;++i) assert(vad.process(small.data(),small.size())==Event::None);
  assert(vad.process(small.data(),small.size())==Event::Started);

  // Perfectly steady noise starts an utterance, then is thrown away after 3 s.
  vad.reset(); start(vad);
  for (int i=0;i<149;++i) assert(frame(vad,2000)==Event::None);
  assert(frame(vad,2000)==Event::Rejected);
  assert(!vad.speaking());
  // The floor now sits above it: the same hum cannot start another utterance...
  for (int i=0;i<30;++i) assert(frame(vad,2000)==Event::None);
  // ...but a person speaking over it still can.
  for (int i=0;i<4;++i) assert(frame(vad,6000)==Event::None);
  assert(frame(vad,6000)==Event::Started);
  muse::Vad fresh;  // the hum above raised this one's floor
  // Servo whine while the head turns (hold) never starts an utterance...
  for (int i=0;i<40;++i) assert(fresh.process(std::array<int16_t,320>{}.data(),320,true)==Event::None);
  std::array<int16_t,320> whine{}; whine.fill(2000);
  for (int i=0;i<40;++i) assert(fresh.process(whine.data(),whine.size(),true)==Event::None);
  assert(!fresh.speaking());
  // ...and leaves no half-built onset behind: speech after it needs its full 100 ms.
  for (int i=0;i<4;++i) assert(frame(fresh,2000)==Event::None);
  assert(frame(fresh,2000)==Event::Started);
  // A dip inside a soft word no longer restarts the 100 ms onset from nothing.
  fresh.reset();
  for (int i=0;i<4;++i) assert(frame(fresh,2000)==Event::None);
  assert(frame(fresh,100)==Event::None);
  for (int i=0;i<2;++i) assert(frame(fresh,2000)==Event::None);
  assert(frame(fresh,2000)==Event::Started);

  // The wake-opened turn waits 4 s of silence, and the silence is not uploaded.
  muse::Vad patient; patient.setPauseFrames(64000);
  start(patient);
  for (int i=0;i<30;++i) assert(frame(patient,static_cast<int16_t>(1500+(i%9)*300))==Event::None);
  for (int i=0;i<199;++i) assert(frame(patient,100)==Event::None);  // 3.98 s: still waiting
  assert(frame(patient,2000)==Event::None);                        // a late word restarts the wait
  for (int i=0;i<199;++i) assert(frame(patient,100)==Event::None);
  assert(frame(patient,100)==Event::Ended);                        // 4.00 s
  assert(patient.excessQuiet()==64000-muse::Vad::kTailFrames);
  // A pause shorter than the tail cannot be configured away.
  muse::Vad floor; floor.setPauseFrames(1); start(floor);
  for (int i=0;i<12;++i) assert(frame(floor,100)==Event::None);
  assert(frame(floor,100)==Event::Ended);

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
