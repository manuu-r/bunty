#pragma once

#include <driver/i2s_types.h>
#include <stdint.h>
#include <time.h>

// Every board wiring detail, protocol constant, and behavioural tunable for
// Bunty. Nothing here allocates or depends on the rest of the firmware, so a
// new enclosure or panel revision is a change to this file alone.
namespace cfg {

constexpr uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return static_cast<uint16_t>(((red & 0xF8) << 8) | ((green & 0xFC) << 3) |
                               (blue >> 3));
}

// ---------------------------------------------------------------- wiring ---

namespace pin {
// Waveshare ESP32-S3-Touch-LCD-2. GPIO4 is the only pin left free.
constexpr int kTftCs = 45;
constexpr int kTftDc = 42;
constexpr int kTftRst = -1;        // no reset line; the driver resets in software
constexpr int kTftBacklight = 1;   // active-high, via an NPN on the board
constexpr int kTftMosi = 38;
constexpr int kTftSclk = 39;

constexpr int kTouchSda = 48;
constexpr int kTouchScl = 47;
constexpr int kTouchInterrupt = 46;

constexpr int kTurnServo = 9;   // SG90, head yaw
constexpr int kTiltServo = 14;  // SG90, head pitch

// MAX98357 amplifier. UART0's pads are free for it: Serial runs over native
// USB. SD is held low, shutting the amp down, except while Bunty is speaking.
constexpr int kAmpShutdown = 2;
constexpr int kAmpBclk = 15;
constexpr int kAmpLrc = 43;
constexpr int kAmpDin = 44;

// Both mics share this bus; opposite L/R straps put each on its own half-frame.
constexpr int kMicBclk = 11;
constexpr int kMicWs = 12;
constexpr int kMicData = 13;
}  // namespace pin

// Give the amplifier and the microphones their own I2S controller each.
constexpr int kSpeakerPort = I2S_NUM_0;
constexpr int kMicrophonePort = I2S_NUM_1;

// --------------------------------------------------------------- display ---

namespace ui {
constexpr int16_t kScreenWidth = 240;
constexpr int16_t kScreenHeight = 320;
// Everything below the persistent status bar belongs to Bunty's face.
constexpr int16_t kStatusBarHeight = 32;
constexpr uint32_t kSpiHz = 40000000;
constexpr uint8_t kBacklightChannel = 0;
constexpr uint32_t kBacklightPwmHz = 12000;
// A faint ~3% PWM glow: enough to see the sleeping eyes and know Bunty is on
// without lighting the room or desk.
constexpr uint8_t kSleepBacklightDuty = 8;
// Temporary bench check: at boot, show each emotion's face for 5 s, forever.
constexpr bool kFaceDemo = false;
// Bunty's screen displays expressive face, pictures, and structured UI cards
// (calendar agenda, weather/stock cards, task lists, and notes).
constexpr bool kTextContent = true;
constexpr uint32_t kPairingActivityFrameMs = 420;
constexpr uint32_t kIdleSleepMs = 120000;

constexpr uint16_t kBackground = rgb565(6, 8, 15);
constexpr uint16_t kSurface = rgb565(17, 24, 23);
constexpr uint16_t kBorder = rgb565(38, 48, 44);
constexpr uint16_t kText = rgb565(233, 241, 223);
constexpr uint16_t kMuted = rgb565(129, 149, 132);
constexpr uint16_t kAccent = rgb565(193, 225, 156);
constexpr uint16_t kSuccess = rgb565(193, 225, 156);
constexpr uint16_t kDanger = rgb565(255, 102, 118);
}  // namespace ui

// ---------------------------------------------------------------- servos ---

namespace servo {
constexpr bool kEnabled = true;
// Temporary bench check: at boot, sweep yaw then pitch fast, then home.
constexpr bool kBootDemo = false;
constexpr float kBootDemoDegreesPerSecond = 360.0f;
constexpr float kBootDemoSweepDegrees = 70.0f;
constexpr bool kTurnEnabled = true;
// Both axes are wired on this carrier, each with an SG90.
constexpr bool kTiltEnabled = true;
constexpr uint8_t kTurnChannel = 2;
constexpr uint8_t kTiltChannel = 3;
constexpr uint8_t kTurnHomeAngle = 100;
// The tilt linkage is mounted in the same orientation as the turn linkage:
// matching targets make Bunty look right/up, then left/down, in one gesture.
// Flip this calibration value later if the physical linkage is reversed.
constexpr uint8_t kTiltHomeAngle = 100;
// A calibrated home must leave this much travel either side of it.
constexpr float kSweepDegrees = 40.0f;
// Default glide rate for ordinary moves: slow enough that 50 Hz position
// updates stay under half a degree, avoiding visible stepping in hobby servos.
constexpr float kSweepDegreesPerSecond = 30.0f;
}  // namespace servo

// ----------------------------------------------------------------- touch ---

namespace touch {
// Hynitron CST816-family controller on the shared touch/IMU I2C bus.
constexpr uint8_t kAddress = 0x15;
constexpr uint32_t kBusHz = 400000;
constexpr uint32_t kLongPressMs = 650;
// A press that wanders further than this is a drag, not a tap.
constexpr int16_t kSlopPixels = 24;
constexpr uint32_t kDebounceMs = 120;
// The controller's auto-sleep default is re-disabled this often, so a glitch
// cannot leave the panel permanently unresponsive.
constexpr uint32_t kReconfigureMs = 4000;
constexpr uint32_t kSampleMs = 15;
// The panel is mounted 180 degrees from the controller's origin: a touch at
// the bottom left reports as the top right.
constexpr bool kSwapXY = false;
constexpr bool kInvertX = true;
constexpr bool kInvertY = true;
}  // namespace touch

// ----------------------------------------------------------------- power ---

// Bunty runs from a 1500 mAh pack through a buck-boost converter rated for 2 A.
// Nothing downstream is measured, so these are modelled worst-case figures used
// to keep concurrent loads under the converter's ceiling. Measure your own
// build and correct them; too low browns out mid-gesture, too high refuses
// motion that would have been fine.
namespace power {
// The converter is rated 2 A; 1500 mA is the design ceiling Bunty holds itself
// to, so the converter is never asked for its last 25%.
constexpr uint16_t kRailMilliamps = 1500;
// Wi-Fi and BLE transmit whenever the stack decides to. Nothing can gate those
// bursts, so the budget simply never spends this much.
constexpr uint16_t kReserveMilliamps = 350;
constexpr uint16_t kBaseMilliamps = 300;      // module, panel, radio average
constexpr uint16_t kBacklightMilliamps = 80;  // at full duty
constexpr uint16_t kServoMilliamps = 750;     // SG90 under load
constexpr uint16_t kAudioMilliamps = 0;       // No MAX98357 amplifier/speaker: pure silent pantomime companion
// Both servos while gesturing/dancing: small arcs at a capped glide rate, so
// neither ever approaches the stall current kServoMilliamps budgets for.
constexpr uint16_t kDanceMilliamps = 440;

// The policy falls out of this arithmetic rather than out of special cases:
// exactly one full-travel servo runs at a time, and the dance fits beside audio.
static_assert(kBaseMilliamps + kBacklightMilliamps + kServoMilliamps <=
                  kRailMilliamps - kReserveMilliamps,
              "one servo must fit inside the rail budget");
static_assert(kBaseMilliamps + kBacklightMilliamps + kAudioMilliamps +
                      kDanceMilliamps <=
                  kRailMilliamps - kReserveMilliamps,
              "the speech dance must fit beside the speaker");
static_assert(kBaseMilliamps + kBacklightMilliamps + 2 * kServoMilliamps >
                  kRailMilliamps - kReserveMilliamps,
              "two servos at once must not fit inside the rail budget");

// Quarter-dBm; 52 is 13 dBm, roughly halving the TX burst the reserve covers.
constexpr int8_t kWifiTxPowerQuarterDbm = 52;
// Streaming voice uplink needs more; 68 is 17 dBm.
constexpr int8_t kVoiceTxPowerQuarterDbm = 68;

// APB stays at 80 MHz across these, so servo LEDC and display SPI are unaffected.
constexpr uint32_t kIdleAfterMs = 30000;
constexpr uint8_t kIdleCpuMhz = 80;
constexpr uint8_t kActiveCpuMhz = 240;

constexpr uint32_t kConservativeMs = 10 * 60 * 1000;
constexpr uint8_t kConservativeBacklight = 160;
constexpr float kConservativeServoDegreesPerSecond = 20.0f;

}  // namespace power

// ----------------------------------------------------------------- audio ---

namespace audio {
// The gateway's only format in both directions: signed 16-bit mono PCM.
constexpr uint32_t kSampleRate = 16000;

// INMP441-style mics put 24 bits left-aligned in a 32-bit slot. Shifting by
// 13 rather than 16 is 18 dB of gain, the level the gateway transcribed well.
constexpr int kMicShift = 13;
// A slot is treated as unwired once its energy falls below 1/8 of the other's;
// otherwise the two mics are averaged, which lowers uncorrelated noise.
constexpr uint32_t kMicSlotRatio = 8;
constexpr size_t kCaptureRingBytes = 256000;  // 8 s: covers a slow Muse uplink setup
// Wake-word tap: 6 bytes a frame (mono, left, right), 1 s of slack.
constexpr size_t kTapRingBytes = 6 * kSampleRate;

constexpr uint8_t kVolumePercent = 100;
constexpr int kSpeakerDmaBuffers = 4;
constexpr int kSpeakerDmaFrames = 240;
constexpr uint32_t kSpeakerDrainMs =
    kSpeakerDmaBuffers * kSpeakerDmaFrames * 1000 / kSampleRate + 30;
// Output RMS that reads as "fully open" for the mouth and the dance.
constexpr float kLevelFullScale = 5000.0f;
}  // namespace audio

// ------------------------------------------------------------ wake word ---

// "Hey Buddy" wakes Bunty from any distance the mics hear, turns its head toward
// the talker, and opens a conversation. Detection is Espressif's stock
// wn10_en_heybuddy model, read from the "model" partition; the direction comes
// from the arrival-time difference between the two microphones.
namespace wake {
constexpr bool kEnabled = true;
// Open the conversation as well as turning. False only turns the head.
constexpr bool kStartConversation = true;
// Centre-to-centre distance of the two capsules: 20 mm, as built. A wrong value
// scales every bearing (too small turns Bunty too far, too large not far enough).
constexpr float kMicSpacingMeters = 0.02f;
// The left I2S slot (J8, L/R pin on GND) is the microphone on Bunty's left.
// Set false if J8 ends up on Bunty's right.
constexpr bool kLeftSlotIsBuntyLeft = true;
// The microphones ride on the head, so a bearing is relative to where it points.
// -1 because on this build a positive servo angle turns Bunty to its left (the
// first test turned the wrong way with +1).
constexpr float kRightIsPositiveYaw = -1.0f;
// The head's servos are limited to this swing either side of home.
constexpr float kMaxTurnDegrees = 40.0f;
// Weakest accepted correlation between the two mics; below it the phrase was
// heard but its direction was not, and Bunty listens without turning.
constexpr float kMinBearingConfidence = 0.2f;
// Ignore bearings smaller than this: the estimate is no better than ~10 degrees.
constexpr float kTurnDeadbandDegrees = 8.0f;
// The turn a wake word opens waits this long: it ends 4 s after you stop speaking, and
// gives up (sending nothing) if you say nothing at all for 4 s. Turns that follow a
// reply keep the quick 660 ms pause.
constexpr uint32_t kSilenceEndMs = 4000;
constexpr uint32_t kHistoryMs = 1000;      // audio kept for the direction estimate
constexpr uint32_t kRefractoryMs = 2500;   // ignore a repeat this soon after a wake
// Listening for the phrase keeps the CPU at full speed: WakeNet needs it, so the
// idle down-clock is off while armed. Costs battery; set kEnabled false to stop.
constexpr uint32_t kBusyRefreshMs = 1000;
}  // namespace wake

// ----------------------------------------------------------------- voice ---

namespace voice {
#ifndef IOT_GATEWAY_HOST
#define IOT_GATEWAY_HOST "bunty.underdogthinkers.com"
#endif
constexpr char kGatewayHost[] = IOT_GATEWAY_HOST;
// Noise suppression and a spectral speech detector clean every utterance before it
// is sent. Only speech, plus a short natural margin, leaves the device; silence,
// room noise and long pauses are dropped. False sends the microphone audio as heard.
constexpr bool kFilterNoise = true;
// WebRTC VAD aggressiveness 0-4 (vad_mode_t): higher drops more non-speech but is
// likelier to clip quiet speech. UNTUNED on Bunty's microphones.
constexpr int kSpeechVadMode = 1;
constexpr uint32_t kGateEdgeMs = 120;    // natural margin kept either side of speech
constexpr uint32_t kMinSpeechMs = 250;   // less speech than this is noise: nothing is sent
constexpr uint32_t kFaceFrameMs = 25;
constexpr uint32_t kInputFullScaleRms = 1500;
}  // namespace voice

// ----------------------------------------------------------------- dance ---

// Bunty's head while it talks: the emotion of the sentence being spoken picks a
// gesture pattern of yaw and tilt oscillation around a leaning rest pose, and
// the loudness of the sound scales it, so the head settles between phrases.
namespace dance {
// Smooth patterns glide on a sine; snappy ones jump between the two extremes
// as fast as `rate` allows and hold there.
struct Style {
  float yawAmplitude, yawHz, tiltAmplitude, tiltHz;  // degrees, cycles per second
  float tilt, yaw;                                   // rest pose offsets, degrees
  float rate;                                        // top speed, degrees per second
  float snap;                                        // 0 smooth .. 1 snappy
};
// The small-arc power reservation; every pose below must fit inside it.
constexpr float kMaxYawDegrees = 18.0f;
constexpr float kMaxTiltDegrees = 15.0f;
constexpr float kMaxDegreesPerSecond = 60.0f;
// Indexed by Emotion.
constexpr Style kStyles[] = {
    {6.0f, 0.35f, 3.0f, 0.70f, 0.0f, 0.0f, 30.0f, 0.0f},     // neutral
    {5.0f, 0.22f, 2.5f, 0.30f, -4.0f, 0.0f, 20.0f, 0.0f},    // soft: slow sway, head low
    {9.0f, 0.60f, 5.0f, 1.40f, 5.0f, 0.0f, 45.0f, 0.0f},     // happy: bouncy, head up
    {12.0f, 0.90f, 6.0f, 1.90f, 4.0f, 0.0f, 60.0f, 0.2f},    // excited: big and fast
    {8.0f, 1.20f, 5.0f, 1.60f, -7.0f, 0.0f, 60.0f, 1.0f},    // angry: sharp head jerks
    {4.0f, 0.18f, 2.0f, 0.22f, -11.0f, 0.0f, 18.0f, 0.0f},   // sad: slow, low, still
    {5.0f, 0.30f, 3.0f, 0.45f, 9.0f, 11.0f, 30.0f, 0.0f},    // curious: head cocked
    {9.0f, 0.70f, 4.0f, 0.70f, 8.0f, -9.0f, 50.0f, 0.7f},    // sassy: chin up, flicks
};
// Fraction of the way to a new style per tick: about 300 ms to settle.
constexpr float kStyleBlend = 0.07f;
// Speech loudness: the head lifts with it, and keeps this much sway at rest.
constexpr float kBeatBlend = 0.25f;
constexpr float kBeatTiltDegrees = 3.0f;
constexpr float kRestGain = 0.35f;
constexpr float kListenTiltDegrees = 15.0f;   // curious head cock while listening
constexpr float kThinkTurnDegrees = -14.0f;   // glance aside while thinking
constexpr float kThinkTiltDegrees = 12.0f;
constexpr float kWorkSweepDegrees = 4.0f;     // scanning while Muse works on a task
constexpr float kWorkSweepHz = 0.35f;
constexpr float kPoseDegreesPerSecond = 60.0f;

constexpr bool stylesFit() {
  for (const Style &s : kStyles) {
    const float yaw = (s.yaw < 0.0f ? -s.yaw : s.yaw) + s.yawAmplitude;
    if (yaw > kMaxYawDegrees || s.rate > kMaxDegreesPerSecond) return false;
    // Loudness only ever lifts the head.
    if (s.tilt + s.tiltAmplitude + kBeatTiltDegrees > kMaxTiltDegrees) return false;
    if (s.tilt - s.tiltAmplitude < -kMaxTiltDegrees) return false;
  }
  return true;
}
static_assert(stylesFit(), "a style clipped by the power reservation loses its character");
constexpr uint32_t kTickMs = 20;
}  // namespace dance

// --------------------------------------------------------------- network ---

namespace net {
// TLS certificate validity depends on a real clock.
constexpr time_t kMinimumTrustedUnixTime = 1704067200;  // 2024-01-01 UTC
constexpr uint32_t kClockSyncRetryMs = 30000;
}  // namespace net

// ------------------------------------------------------- persisted state ---

// Local power state and the gateway TTS credential. Muse owns its homehub NVS.
namespace nvs {
constexpr char kNamespace[] = "bunty";

constexpr char kBrownouts[] = "brownouts";

constexpr char kIotToken[] = "iot_token";
constexpr char kIotDevice[] = "iot_device";

}  // namespace nvs

}  // namespace cfg
