#include "SpeechGate.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

using muse::kGateFrameSamples;
namespace {
// Each frame carries its own number, so the output shows exactly which survived.
struct Run {
  std::array<int16_t, 12 * kGateFrameSamples> storage{};
  muse::SpeechGate gate{storage.data(), 12};
  std::vector<int> kept;
  void push(int id, bool speech) {
    std::array<int16_t, kGateFrameSamples> frame;
    frame.fill(static_cast<int16_t>(id));
    gate.push(frame.data(), speech, [&](const int16_t *p, size_t n) {
      assert(n == kGateFrameSamples);
      kept.push_back(p[0]);
    });
  }
};
}  // namespace

int main() {
  {  // Silence and noise on their own produce nothing.
    Run r;
    for (int i = 0; i < 1000; ++i) r.push(i, false);
    assert(r.kept.empty() && !r.gate.started() && r.gate.speechFrames() == 0);
  }
  {  // 5 s of room noise, a word, then more noise: only the word and 120 ms either side survive.
    Run r;
    for (int i = 0; i < 500; ++i) r.push(i, false);          // frames 0..499
    for (int i = 500; i < 530; ++i) r.push(i, true);          // 300 ms of speech
    for (int i = 530; i < 900; ++i) r.push(i, false);
    assert(r.gate.speechFrames() == 30);
    assert(r.kept.size() == 12 + 30 + 12);
    assert(r.kept.front() == 488 && r.kept[11] == 499 && r.kept[12] == 500);  // newest 12 quiet frames before
    assert(r.kept[41] == 529 && r.kept[42] == 530 && r.kept.back() == 541);   // 12 quiet frames after
  }
  {  // A short pause between words is kept whole, so speech keeps its rhythm.
    Run r;
    for (int i = 0; i < 10; ++i) r.push(i, true);
    for (int i = 10; i < 30; ++i) r.push(i, false);           // 200 ms pause (< 2 x 120 ms)
    for (int i = 30; i < 40; ++i) r.push(i, true);
    assert(r.kept.size() == 40);
    for (int i = 0; i < 40; ++i) assert(r.kept[i] == i);
  }
  {  // A long pause is squeezed to 12 + 12 frames, and nothing out of order or twice.
    Run r;
    for (int i = 0; i < 10; ++i) r.push(i, true);
    for (int i = 10; i < 410; ++i) r.push(i, false);          // 4 s of dead air
    for (int i = 410; i < 420; ++i) r.push(i, true);
    assert(r.kept.size() == 10 + 12 + 12 + 10);
    assert(r.kept[10] == 10 && r.kept[21] == 21);             // tail of the first word
    assert(r.kept[22] == 398 && r.kept[33] == 409);           // lead-in to the second
    assert(r.kept[34] == 410 && r.kept.back() == 419);
    assert(r.gate.speechFrames() == 20);
  }
  {  // A pause of exactly the edge length emits each frame once.
    Run r;
    r.push(0, true);
    for (int i = 1; i <= 12; ++i) r.push(i, false);
    r.push(13, true);
    assert(r.kept.size() == 14);
    for (int i = 0; i < 14; ++i) assert(r.kept[i] == i);
    // 13 quiet frames: the 13th is held, then replayed ahead of the word.
    Run q;
    q.push(0, true);
    for (int i = 1; i <= 13; ++i) q.push(i, false);
    q.push(14, true);
    assert(q.kept.size() == 15 && q.kept[13] == 13 && q.kept[14] == 14);
  }
  {  // reset() makes it a new utterance.
    Run r;
    r.push(0, true); r.push(1, false);
    r.gate.reset(); r.kept.clear();
    for (int i = 0; i < 50; ++i) r.push(i, false);
    assert(r.kept.empty() && r.gate.speechFrames() == 0);
  }
  {  // The ring replays in order and wraps.
    std::array<int16_t, 4 * kGateFrameSamples> storage{};
    muse::FrameRing ring(storage.data(), 4);
    std::array<int16_t, kGateFrameSamples> frame;
    for (int i = 0; i < 7; ++i) { frame.fill(static_cast<int16_t>(i)); ring.push(frame.data(), i % 2); }
    std::vector<int> ids; std::vector<bool> verdicts;
    ring.flush([&](const int16_t *p, bool speech) { ids.push_back(p[0]); verdicts.push_back(speech); });
    assert((ids == std::vector<int>{3, 4, 5, 6}));
    assert((verdicts == std::vector<bool>{true, false, true, false}));
    ids.clear();
    ring.flush([&](const int16_t *p, bool) { ids.push_back(p[0]); });
    assert(ids.empty());
  }
  std::puts("Speech gate: leading/trailing silence, pause squeezing, margins, order, reset and frame ring passed");
}
