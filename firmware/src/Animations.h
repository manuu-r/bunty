#pragma once

#include "Runtime.h"
#include <stddef.h>

#include "Emotion.h"
#include "Face.h"

class Display;

// Owns Bunty's matte, expressive face states drawn below Bunty's persistent 32 px
// status bar. Callers serialize access to the shared Flow32 Display before
// invoking these methods; the class owns frame selection, timing, and clipped
// partial presentation.
class BuntyAnimations {
 public:
  static constexpr uint32_t wakeDurationMs() { return 1540; }

  explicit BuntyAnimations(Display &display) : display_(display) {}

  // The first speaking frame can be composed into a larger full-screen frame.
  void beginSpeaking(uint32_t now, bool present = true);
  // Speaking eyes and the mouth take this emotion's shapes on
  // their next frame.
  void setEmotion(Emotion emotion) { emotion_ = emotion; }
  void setAttention(Face::Attention attention) {face_.setAttention(attention);}
  void serviceSpeaking(uint32_t now);

  // The mouth below the eyes. `openness` is 0-100 and tracks live speech
  // loudness. Only the mouth band is redrawn, so it composes over the eyes.
  void drawSpeakingMouth(uint8_t openness,uint8_t opacity=255);
  void invalidateMouth(){face_.invalidateMouth();}

  // A microphone glyph and phase label ("LISTENING", "THINKING") between the
  // status bar and the eyes. Both calls repaint only that band.
  void drawWakeIcon(const char *label);
  void clearWakeIcon();

  // Punchy 1-to-3 word reaction text or emoji below the eyes.
  void drawReaction(const char *text);
  void clearReaction();

  // Progress status and bar during long-running tasks between the status bar and the eyes.
  void drawProgress(const char *status, int percent, uint32_t now);
  void clearProgress();

  // A scrolling level history along the bottom edge, drawn while Bunty is
  // capturing speech. `levels` is a ring of 0-100 values; `head` is the index
  // the next sample would occupy, so the oldest bar is drawn leftmost.

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
