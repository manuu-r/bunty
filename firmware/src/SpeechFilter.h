#pragma once

#include <cstdint>

#include "SpeechGate.h"

// Espressif's WebRTC noise suppressor followed by its spectral speech detector,
// on 10 ms frames. The suppressor takes steady background (fans, hum, room
// tone) out of the audio; the detector then says whether what is left is a
// voice, which an energy threshold alone cannot.
class SpeechFilter {
 public:
  ~SpeechFilter();
  // False leaves the filter in pass-through: audio unchanged, everything
  // reported as speech, so the conversation still works without it.
  bool begin();
  bool active() const { return noise_ && vad_; }
  // kGateFrameSamples in, kGateFrameSamples cleaned out; returns true for speech.
  bool process(const int16_t *raw, int16_t *clean);

 private:
  void *noise_ = nullptr;
  void *vad_ = nullptr;
  int16_t scratch_[muse::kGateFrameSamples] = {};
};
