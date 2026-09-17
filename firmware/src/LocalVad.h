#pragma once

#include <fvad.h>
#include "VoiceActivity.h"

// One allocation at begin(), no allocation in reset()/process(). The device
// calls begin inside InternalAllocationScope; only voice-up touches the engine
// after startup. PCM remains unmodified for upload.
class LocalVad {
 public:
  bool begin() {
    if (!engine_) engine_ = fvad_new();
    return engine_ && reset();
  }
  void end() {
    if (engine_) fvad_free(engine_);
    engine_ = nullptr;
  }
  bool reset() {
    if (!engine_) return false;
    fvad_reset(engine_);  // Also resets mode/rate, so restore BOTH each time.
    return fvad_set_mode(engine_, cfg::voice::kVadMode) == 0 &&
           fvad_set_sample_rate(engine_, cfg::voice::kVadSampleRate) == 0;
  }
  int process(const int16_t *samples, size_t count) {
    if (!engine_ || !samples || count != cfg::voice::kFrameSamples) return -1;
    return fvad_process(engine_, samples, count);
  }

 private:
  Fvad *engine_ = nullptr;
};
