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
// Everything below the persistent status bar belongs to the dashboard or to
// Bunty's face.
constexpr int16_t kStatusBarHeight = 32;
constexpr uint32_t kSpiHz = 40000000;
constexpr uint8_t kBacklightChannel = 0;
constexpr uint32_t kBacklightPwmHz = 12000;
// A faint ~3% PWM glow: enough to see the sleeping eyes and know Bunty is on
// without lighting the room or desk.
constexpr uint8_t kSleepBacklightDuty = 8;
// Temporary bench check: at boot, show each emotion's face for 5 s, forever.
constexpr bool kFaceDemo = false;

constexpr uint16_t kBackground = rgb565(6, 8, 15);
constexpr uint16_t kSurface = rgb565(17, 21, 33);
constexpr uint16_t kBorder = rgb565(38, 44, 62);
constexpr uint16_t kText = rgb565(246, 248, 252);
constexpr uint16_t kMuted = rgb565(142, 151, 172);
constexpr uint16_t kAccent = rgb565(111, 97, 255);
constexpr uint16_t kSuccess = rgb565(88, 226, 173);
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
constexpr uint32_t kPairingDecisionTimeoutMs = 30000;
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
constexpr uint16_t kAudioMilliamps = 300;     // MAX98357 into 4 ohm at speech volume
// Both servos while dancing to speech: small arcs at a capped glide rate, so
// neither ever approaches the stall current kServoMilliamps budgets for.
constexpr uint16_t kDanceMilliamps = 440;
constexpr uint16_t kBatteryMilliampHours = 1500;

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
constexpr uint32_t kIdleAfterMs = 1500;
constexpr uint8_t kIdleCpuMhz = 80;
constexpr uint8_t kActiveCpuMhz = 240;

constexpr uint32_t kConservativeMs = 10 * 60 * 1000;
constexpr uint8_t kConservativeBacklight = 160;
constexpr float kConservativeServoDegreesPerSecond = 20.0f;

constexpr uint8_t kSaverBacklight = 60;
// Disabled by default: the gauge has no feedback and drifts.
constexpr uint8_t kSaverPercent = 0;
constexpr uint32_t kFuelPersistMs = 60000;
}  // namespace power

// --------------------------------------------------------------- pairing ---

namespace pairing {
constexpr char kAccessoryProtocol[] = "dev.gauge.accessory";
constexpr char kPairingProtocol[] = "dev.gauge.pairing";
constexpr char kCommissionProtocol[] = "dev.gauge.commission";
constexpr uint16_t kProtocolVersion = 1;
constexpr char kFirmwareVersion[] = "0.1.0";

constexpr char kServiceUuid[] = "c9cce6f3-bf10-4e6d-b719-f32911bbba89";
constexpr char kIdentityCharacteristicUuid[] =
    "1db9634c-a20f-43c6-8ed9-69ceda338178";
constexpr char kConfigCharacteristicUuid[] =
    "289be295-d110-411b-888c-c80a601177fa";
constexpr char kStatusCharacteristicUuid[] =
    "e763eccb-fa4c-4e3a-9211-850513371105";

// Commissioning arrives as magic-tagged, sequenced GATT writes.
constexpr uint8_t kFrameMagic = 0x47;
constexpr size_t kFrameHeaderSize = 3;
constexpr size_t kMaximumConfigBytes = 4335;
constexpr uint16_t kMtu = 185;

constexpr uint32_t kWifiJoinTimeoutMs = 30000;
// Bluedroid owns a large share of internal heap. Hold the "connected" screen
// briefly, then release it for Wi-Fi and the dashboard.
constexpr uint32_t kShutdownDelayMs = 2500;
constexpr uint32_t kActivityFrameMs = 420;
}  // namespace pairing

// ------------------------------------------------------------- dashboard ---

namespace dashboard {
constexpr char kProtocol[] = "dev.gauge.dashboard";
constexpr char kServiceType[] = "_gauge._tcp.local.";
constexpr char kDefaultPath[] = "/v1/dashboard";
constexpr size_t kMaximumBytes = 12288;

constexpr uint32_t kPageTimeMs = 11000;
constexpr uint32_t kDefaultRefreshMs = 120000;
constexpr uint32_t kMinimumRefreshSeconds = 30;
constexpr uint32_t kMaximumRefreshSeconds = 900;
constexpr uint32_t kRetryMs = 15000;
// ESP32 modem sleep can leave a long-lived LAN accessory with a stale ARP or
// station route even though WiFi.status() still reports connected. Force one
// clean station reconnect after this many consecutive TCP failures.
constexpr uint8_t kReconnectAfterFailures = 3;
constexpr uint32_t kIdleSleepMs = 120000;

constexpr uint32_t kConnectTimeoutMs = 4000;
constexpr uint32_t kReadTimeoutMs = 5000;
constexpr uint32_t kUnpairNotifyConnectMs = 2000;
constexpr uint32_t kUnpairNotifyReadMs = 2500;

// Rows that fit on each page at the current font sizes.
constexpr size_t kProviderRowsPerPage = 4;
constexpr size_t kLimitRowsPerPage = 6;
constexpr size_t kEventRowsPerPage = 5;
constexpr size_t kTodoRowsPerPage = 6;
}  // namespace dashboard

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
constexpr size_t kCaptureRingBytes = 256000;  // 8 s: covers a slow TLS + Flux setup

constexpr uint8_t kVolumePercent = 95;
constexpr int kSpeakerDmaBuffers = 4;
constexpr int kSpeakerDmaFrames = 240;
constexpr uint32_t kSpeakerDrainMs =
    kSpeakerDmaBuffers * kSpeakerDmaFrames * 1000 / kSampleRate + 30;
// Aura streams faster than real time; when this fills, the socket simply
// stops reading and TCP holds the rest at the gateway. ~12 s of reply.
constexpr size_t kPlaybackRingBytes = 384 * 1024;
// Jitter cushion before the first sample, and again after any underrun.
constexpr size_t kPrebufferBytes = kSampleRate * 2 * 120 / 1000;
// Output RMS that reads as "fully open" for the mouth and the dance.
constexpr float kLevelFullScale = 5000.0f;
}  // namespace audio

// ----------------------------------------------------------------- voice ---

namespace voice {
#ifndef IOT_GATEWAY_HOST
#define IOT_GATEWAY_HOST "bunty.underdogthinkers.com"
#endif
constexpr char kGatewayHost[] = IOT_GATEWAY_HOST;
constexpr char kLivePath[] = "/v1/live";

// Flux's recommended chunk: 80 ms of 16 kHz linear16.
constexpr size_t kUploadFrameBytes = 2560;

// Conversation mode ends after this long without anyone talking. Gateway
// speech events are authoritative; local energy only bridges the few hundred
// ms Flux needs to report a word that starts right at the deadline.
constexpr uint32_t kSilenceExitMs = 8000;
constexpr uint32_t kLocalSpeechGraceMs = 3000;
constexpr uint32_t kLocalSpeechHoldMs = 600;

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kReplyTimeoutMs = 30000;
constexpr uint32_t kCompleteTimeoutMs = 5000;
// IDF drops the connection when a write times out, so give a slow uplink room.
constexpr uint32_t kSendTimeoutMs = 5000;
constexpr uint8_t kMaxConsecutiveFailures = 3;
constexpr uint32_t kRetryDelayMs = 800;

constexpr int kSocketBufferBytes = 4096;
constexpr int kSocketTaskStack = 6144;
constexpr size_t kControlBytes = 8192;

constexpr uint32_t kFaceFrameMs = 45;
constexpr size_t kWaveformSamples = 58;
constexpr uint32_t kWaveformFullScaleRms = 1500;
}  // namespace voice

// ----------------------------------------------------------------- dance ---

// Bunty's head while it talks is driven by the emotion of the sentence being
// spoken, not by the sound: each emotion is a gesture pattern of yaw and tilt
// oscillation around a leaning rest pose.
namespace dance {
// Smooth patterns glide on a sine; snappy ones jump between the two extremes
// as fast as `rate` allows and hold there.
struct Style {
  float yawAmplitude, yawHz, tiltAmplitude, tiltHz;  // degrees, cycles per second
  float tilt, yaw;                                   // rest pose offsets, degrees
  float rate;                                        // top speed, degrees per second
  float snap;                                        // 0 smooth .. 1 snappy
};
// Indexed by Emotion.
constexpr Style kStyles[] = {
    {40.0f, 0.40f, 15.0f, 0.80f, 0.0f, 0.0f, 300.0f, 0.0f},      // neutral
    {25.0f, 0.25f, 10.0f, 0.35f, -10.0f, 0.0f, 120.0f, 0.0f},    // soft: slow sway, head low
    {50.0f, 0.60f, 22.0f, 1.60f, 12.0f, 0.0f, 450.0f, 0.0f},     // happy: bouncy, head up
    {70.0f, 1.00f, 28.0f, 2.20f, 8.0f, 0.0f, 650.0f, 0.2f},      // excited: big and fast
    {35.0f, 1.50f, 20.0f, 2.00f, -18.0f, 0.0f, 650.0f, 1.0f},    // angry: sharp head jerks
    {18.0f, 0.18f, 8.0f, 0.25f, -25.0f, 0.0f, 70.0f, 0.0f},      // sad: slow, low, still
    {25.0f, 0.30f, 14.0f, 0.45f, 20.0f, 30.0f, 150.0f, 0.0f},    // curious: head cocked
    {45.0f, 0.80f, 15.0f, 0.80f, 18.0f, -25.0f, 550.0f, 0.7f},   // sassy: chin up, flicks
};
// Fraction of the way to a new style per tick: about 300 ms to settle.
constexpr float kStyleBlend = 0.07f;
constexpr float kListenTiltDegrees = 15.0f;   // curious head cock while listening
constexpr float kThinkTurnDegrees = -30.0f;  // glance aside while thinking
constexpr float kThinkTiltDegrees = 12.0f;
constexpr float kPoseDegreesPerSecond = 90.0f;
constexpr uint32_t kTickMs = 20;
}  // namespace dance

// --------------------------------------------------------------- network ---

namespace net {
constexpr uint32_t kMdnsQueryTimeoutMs = 3000;
// TLS certificate validity depends on a real clock.
constexpr time_t kMinimumTrustedUnixTime = 1704067200;  // 2024-01-01 UTC
constexpr uint32_t kClockSyncRetryMs = 30000;
}  // namespace net

// ------------------------------------------------------- persisted state ---

// NVS namespace and keys. Unpairing clears kGaugeKeys only; the IoT gateway
// credential and the Bluetooth address are provisioned once and survive
// re-pairing, so this accessory keeps one identity for its whole life.
namespace nvs {
constexpr char kNamespace[] = "bunty";

constexpr char kApiVersion[] = "api";
constexpr char kDeviceId[] = "device_id";
constexpr char kServerId[] = "server_id";
constexpr char kToken[] = "token";
constexpr char kServiceType[] = "service";
constexpr char kPath[] = "path";
constexpr char kPort[] = "port";
constexpr char kServerIp[] = "server_ip";
constexpr char kWifiSsid[] = "wifi_ssid";
constexpr char kWifiPassword[] = "wifi_pass";
constexpr char kDashboard[] = "dashboard";
constexpr char kPaired[] = "paired";
constexpr char kBleAddress[] = "ble_addr";

constexpr char kBrownouts[] = "brownouts";
constexpr char kFuelUsed[] = "fuel_mas";

constexpr char kIotToken[] = "iot_token";
constexpr char kIotDevice[] = "iot_device";
constexpr char kIotSession[] = "iot_session";

constexpr const char *kGaugeKeys[] = {
    kApiVersion, kDeviceId, kServerId,     kToken,     kServiceType,
    kPath,       kPort, kServerIp,     kWifiSsid,     kWifiPassword, kDashboard,
    kPaired,
};
}  // namespace nvs

}  // namespace cfg
