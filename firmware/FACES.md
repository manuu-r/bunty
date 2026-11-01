# Bunty face refresh

[Expression and attention preview](docs/bunty-faces-preview.png) is rendered by
the real firmware Face and Display code. The top eight cells show the emotion
vocabulary; the bottom row shows actual idle/listening/thinking/speaking poses.
Preview labels are explanatory, not new controls on the device.

The face now uses matte ivory eyes, dark pupils and a soft green mouth. Shape
and eye direction carry the expression. Neutral is attentive, soft is relaxed,
happy uses closed crescents, excited opens wide, angry narrows inward, sad
softens outward, curious raises one eye, and sassy holds a side-eye. There is
no glow, gradient, blush, tear or floating decoration.

Listening opens the eyes slightly and keeps attention centered. Thinking raises
the gaze to the side with asymmetric lids. Idle gaze holds for 3.2–6.2 seconds
between small glances. Blinks close over 80 ms, hold for 40 ms and reopen over
120 ms, sampled by the existing 80 ms eye cadence. The mouth follows measured
speaker output with a release tween; this is amplitude animation, not phoneme
lip sync. Silent thinking continues that tween until closed. Listening keeps the resting mouth and uses the border aura described in
[VOICE.md](VOICE.md). Sleep
uses quiet closed lids; wake unfolds them into the same face.

Eye pixels stay within y=96–217, mouth pixels within y=220–257. The voice face
has no text or waveform bands. Both eyes and mouth use fixed stack row scratch
and return dirty rectangles; the renderer allocates no heap storage.
On this ESP-IDF build the individual eye/mouth frames are 816/592 bytes, excluding
callees. The binary fits with 54% of the application partition free.

Host checks exercise every emotion/attention combination, dirty bounds, caption
preservation, input clamping, held mouth frames and clock wrapping under UB/bounds
sanitizers. The full host suite and ESP-IDF build pass. Preview rendering is visual
verification of the artwork, not live speech timing or physical LCD validation.
Physical LCD and spoken-reply timing need on-device verification.

Reproduce the preview from firmware/:

```sh
tests/host/face_preview.sh docs/bunty-faces-preview.png 0 700
```
