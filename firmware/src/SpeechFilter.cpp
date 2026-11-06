#include "SpeechFilter.h"

#include <esp_ns.h>
#include <esp_vad.h>
#include <string.h>

#include "Config.h"
#include "Log.h"

SpeechFilter::~SpeechFilter() {
  if (noise_) ns_destroy(noise_);
  if (vad_) vad_destroy(static_cast<vad_handle_t>(vad_));
}

bool SpeechFilter::begin() {
  if (active()) return true;
  if (!cfg::voice::kFilterNoise) return false;
  static_assert(muse::kGateFrameSamples == cfg::audio::kSampleRate / 100, "the filter works on 10 ms frames");
  noise_ = ns_create(10);
  vad_ = vad_create(static_cast<vad_mode_t>(cfg::voice::kSpeechVadMode));
  if (!noise_ || !vad_) {
    Serial.println("[voice] noise filter unavailable; sending unfiltered audio");
    if (noise_) ns_destroy(noise_);
    if (vad_) vad_destroy(static_cast<vad_handle_t>(vad_));
    noise_ = vad_ = nullptr;
    return false;
  }
  logPrintf("[voice] noise suppression and speech detector ready (mode %d)\n", cfg::voice::kSpeechVadMode);
  return true;
}

bool SpeechFilter::process(const int16_t *raw, int16_t *clean) {
  if (!active()) {
    memcpy(clean, raw, muse::kGateFrameSamples * sizeof(int16_t));
    return true;
  }
  // The library takes writable buffers.
  memcpy(scratch_, raw, sizeof(scratch_));
  ns_process(noise_, scratch_, clean);
  memcpy(scratch_, clean, sizeof(scratch_));
  return vad_process(static_cast<vad_handle_t>(vad_), scratch_, cfg::audio::kSampleRate, 10) == VAD_SPEECH;
}
