#pragma once
#include "Runtime.h"
#include "SoundBearing.h"

// Where a sound came from, from the arrival-time difference between the two
// microphones (GCC-PHAT). The microphone task feeds windows in; a low-priority
// worker does the FFTs; the main task reads the result. See HEARING.md.
namespace hearing {
// Allocates its buffers and starts the worker. False leaves the feature off and
// the voice path untouched.
bool begin();
bool active();
// Microphone task: matching blocks of both capsules at the capture rate,
// DC-removed and gain-scaled like the voice path.
void push(const int16_t *left, const int16_t *right, size_t frames);
// The audio is no longer continuous (capture stopped, a capsule is unwired).
void gap();
// Main task, every loop while the head moves: the servo's noise would
// corrupt the estimate, so windows are discarded until it settles.
void hold();
// False when no recent window agreed on a direction.
bool bearing(Bearing *out);
// Forget everything heard so far, e.g. after the head has turned.
void reset();
}  // namespace hearing
