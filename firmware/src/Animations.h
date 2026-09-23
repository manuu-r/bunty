#pragma once

#include <Arduino.h>
#include <stddef.h>

#include "Emotion.h"
#include "Face.h"

class Display;

// Owns every chunky cyan robo-eye emotion drawn below Bunty's persistent 32 px
// status bar. Callers serialize access to the shared Flow32 Display before
// invoking these methods; the class owns frame selection, timing, and clipped
// partial presentation.
class BuntyAnimations {
 public:
  static constexpr uint32_t wakeDurationMs() { return 1540; }

  explicit BuntyAnimations(Display &display) : display_(display) {}

  // The first speaking frame can be composed into a larger full-screen frame.
  void beginSpeaking(uint32_t now, bool present = true);
  // Speaking eyes and the mouth take this emotion's shapes and colour on
  // their next frame.
  void setEmotion(Emotion emotion) { emotion_ = emotion; }
  void serviceSpeaking(uint32_t now);

  // The mouth below the eyes. `openness` is 0-100 and tracks live speech
  // loudness. Only the mouth band is redrawn, so it composes over the eyes.
  void drawSpeakingMouth(uint8_t openness);

  // A microphone glyph and phase label ("LISTENING", "THINKING") between the
  // status bar and the eyes. Both calls repaint only that band.
  void drawWakeIcon(const char *label);
  void clearWakeIcon();

  // A scrolling level history along the bottom edge, drawn while Bunty is
  // capturing speech. `levels` is a ring of 0-100 values; `head` is the index
  // the next sample would occupy, so the oldest bar is drawn leftmost.
  void drawInputWaveform(const uint8_t *levels, size_t count, size_t head);
  void clearInputWaveform();

  void beginSleeping(uint32_t now);
  void serviceSleeping(uint32_t now);

  void beginWaking(uint32_t now);
  // Returns false once the wake animation has completed.
  bool serviceWaking(uint32_t now);

 private:
  void drawSpeakingFrame();
  void drawSleepFrame();
  void drawWakeFrame();

  Display &display_;
  uint32_t lastFrameAt_ = 0;
  uint32_t animationStartedAt_ = 0;
  uint8_t frame_ = 0;
  Emotion emotion_ = Emotion::Neutral;
  Face face_;
};
