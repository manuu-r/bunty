#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace muse {

// Everything below works on 10 ms frames of 16 kHz PCM, the unit the noise
// suppressor and the speech detector both use.
constexpr size_t kGateFrameSamples = 160;

// The last few frames before an utterance is confirmed, each with the speech
// detector's verdict, so the words that started it are not lost.
class FrameRing {
 public:
  static constexpr size_t kMaxFrames = 64;
  // `storage` holds capacity * kGateFrameSamples samples.
  FrameRing(int16_t *storage, size_t capacity) : samples_(storage), capacity_(std::min(capacity, kMaxFrames)) {}
  void reset() { next_ = used_ = 0; }
  void push(const int16_t *frame, bool speech) {
    memcpy(samples_ + next_ * kGateFrameSamples, frame, kGateFrameSamples * sizeof(int16_t));
    speech_[next_] = speech;
    next_ = (next_ + 1) % capacity_;
    used_ = std::min(used_ + 1, capacity_);
  }
  // Oldest first; empties the ring.
  template <class Visit> void flush(Visit visit) {
    for (size_t i = 0, at = (next_ + capacity_ - used_) % capacity_; i < used_; ++i, at = (at + 1) % capacity_)
      visit(samples_ + at * kGateFrameSamples, speech_[at]);
    reset();
  }
 private:
  int16_t *samples_;
  size_t capacity_, next_ = 0, used_ = 0;
  bool speech_[kMaxFrames] = {};
};

// Keeps the speech and a natural margin around it, and drops the rest.
//
// Frames the detector calls speech always pass. Quiet frames pass only as the
// first `edge` frames after speech (so word endings are not clipped) and the
// last `edge` before the next speech (so word starts are not). A longer gap is
// therefore squeezed to 2 x edge, and silence before the first word or after the
// last never leaves the device. Because nothing but speech and its margin is
// kept, a TV in another room, a fan between words and the dead air while someone
// thinks cost neither bandwidth nor transcription accuracy.
class SpeechGate {
 public:
  // `storage` holds edge * kGateFrameSamples samples for the pre-speech margin.
  SpeechGate(int16_t *storage, size_t edgeFrames) : ring_(storage), edge_(edgeFrames) {}

  void reset() {
    next_ = used_ = gap_ = speechFrames_ = 0;
    started_ = false;
  }
  // Frames the detector called speech and the gate kept (margins not counted).
  size_t speechFrames() const { return speechFrames_; }
  bool started() const { return started_; }

  // emit(const int16_t *samples, size_t count) is called in order for every
  // frame that is kept.
  template <class Emit> void push(const int16_t *frame, bool speech, Emit emit) {
    if (speech) {
      // The margin before this word: everything held back, newest `edge` of it.
      for (size_t i = 0, at = (next_ + edge_ - used_) % edge_; i < used_; ++i, at = (at + 1) % edge_)
        emit(ring_ + at * kGateFrameSamples, kGateFrameSamples);
      next_ = used_ = gap_ = 0;
      started_ = true;
      ++speechFrames_;
      emit(frame, kGateFrameSamples);
      return;
    }
    if (started_ && gap_ < edge_) {
      ++gap_;
      emit(frame, kGateFrameSamples);  // the tail of the last word
      return;
    }
    ++gap_;
    memcpy(ring_ + next_ * kGateFrameSamples, frame, kGateFrameSamples * sizeof(int16_t));
    next_ = (next_ + 1) % edge_;
    used_ = std::min(used_ + 1, edge_);
  }

 private:
  int16_t *ring_;
  size_t edge_, next_ = 0, used_ = 0, gap_ = 0, speechFrames_ = 0;
  bool started_ = false;
};

}  // namespace muse
