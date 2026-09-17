#pragma once

#include <driver/i2s.h>
#include <stdint.h>
#include "VoiceActivity.h"

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
// The Waveshare ESP32-S3-Touch-LCD-2 differs from the carrier board in two
// spots. Its on-board ST7789T3 (still 240x320) sits on the board's LCD/SD SPI
// bus and has no reset GPIO, so kTftRst is -1 and the driver does a software
// reset. And GPIO5 is not on its P1/P2 headers (it is the battery-ADC
// divider), so the amplifier word-select moves to the spare GPIO18. Every
// other servo/amp/mic pin is unchanged, so the same external harness moves
// between boards.
#if defined(BUNTY_BOARD_WAVESHARE)
constexpr int kTftCs = 45;
constexpr int kTftDc = 42;
constexpr int kTftRst = -1;
constexpr int kTftBacklight = 1;   // active-high, via an NPN on the board
constexpr int kTftMosi = 38;
constexpr int kTftSclk = 39;

constexpr int kAmpLrc = 18;
#else
constexpr int kTftCs = 10;
constexpr int kTftDc = 9;
constexpr int kTftRst = 14;
constexpr int kTftBacklight = 21;
constexpr int kTftMosi = 11;
constexpr int kTftSclk = 12;

constexpr int kAmpLrc = 5;
#endif

constexpr int kTurnServo = 8;
constexpr int kTiltServo = 7;

constexpr int kAmpShutdown = 13;
constexpr int kAmpBclk = 4;
constexpr int kAmpDin = 6;

constexpr int kMicBclk = 15;
constexpr int kMicWs = 16;
constexpr int kMicData = 17;
}  // namespace pin

// The amplifier and the microphone each own an I2S controller for the whole
// run; neither driver is installed on the other's port.
constexpr i2s_port_t kSpeakerPort = I2S_NUM_0;
constexpr i2s_port_t kMicrophonePort = I2S_NUM_1;

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
// GPIO 8 is the verified turn-servo signal. Keep the unresolved GPIO 7 tilt
// path disconnected until its wiring/power issue has been diagnosed.
constexpr bool kTurnEnabled = true;
constexpr bool kTiltEnabled = false;
constexpr uint8_t kTurnChannel = 2;
constexpr uint8_t kTiltChannel = 3;
constexpr uint8_t kTurnHomeAngle = 100;
// The tilt linkage is mounted in the same orientation as the turn linkage:
// matching targets make Bunty look right/up, then left/down, in one gesture.
// Flip this calibration value later if the physical linkage is reversed.
constexpr uint8_t kTiltHomeAngle = 100;
// Bunty looks this far to each side of home before a new dashboard page.
constexpr float kSweepDegrees = 40.0f;
// A full 100 -> 140 -> 60 -> 100 sweep is 160 degrees of travel. A conservative
// rate keeps the 50 Hz position changes below half a degree, avoiding visible
// stepping in small hobby servos while still completing inside the page dwell.
constexpr float kSweepDegreesPerSecond = 30.0f;
}  // namespace servo

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
// briefly, then release Bluetooth before audio claims DMA memory.
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

// ------------------------------------------------------------------ taps ---

namespace tap {
constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kMinimumLevel = 24000;
constexpr uint32_t kNoiseMultiplier = 7;
constexpr uint32_t kRefractoryMs = 150;
constexpr uint32_t kQuietToRearmMs = 70;
// The MEMS microphone's useful speech occupies only a small part of the signed
// 24-bit slot. After converting to linear16, restore 18 dB of level for VAD and
// STT; saturating arithmetic protects close taps and loud speech.
constexpr int32_t kVoiceCaptureGain = 8;
// Roughly 150 ms of audio is enough to tell the live microphone slot from the
// silent one without delaying the first spoken word noticeably.
constexpr uint16_t kSlotProbeReads = 50;

// Wake and unpair are graded by absolute level and by headroom over the
// measured room noise, so a soft knock cannot trigger a destructive gesture.
constexpr uint32_t kWakeMinimumLevel = 60000;
constexpr uint32_t kWakeNoiseMultiplier = 10;
constexpr uint32_t kUnpairMinimumLevel = 180000;
constexpr uint32_t kUnpairNoiseMultiplier = 16;

// Double tap arms voice. A single tap does nothing, so an accidental knock or
// a bump of the desk cannot start a conversation on its own.
constexpr uint32_t kArmDoubleTapWindowMs = 300;
// An enclosure echo arrives within a few tens of milliseconds of the strike;
// requiring a real gap keeps one physical tap from counting as two.
constexpr uint32_t kArmDoubleTapMinimumGapMs = 70;

// Keep destructive taps distinctly slower than the quick double tap that starts
// a conversation. Their timing windows intentionally do not overlap.
constexpr uint32_t kUnpairMinimumGapMs = 450;
constexpr uint32_t kUnpairMaximumGapMs = 1000;
// Unpairing is destructive, so require a clearly intentional rhythm: three firm
// taps, a noticeable pause, then three more firm taps.
constexpr uint32_t kUnpairInterGroupMinimumMs = 1500;
constexpr uint32_t kUnpairInterGroupMaximumMs = 3500;
constexpr uint32_t kUnpairConfirmationGuardMs = 1000;
constexpr uint32_t kUnpairConfirmationTimeoutMs = 6000;
constexpr uint8_t kUnpairTapsPerGroup = 3;

constexpr uint32_t kPairingDecisionTimeoutMs = 30000;
constexpr uint32_t kPairingDoubleTapWindowMs = 700;
}  // namespace tap

// ----------------------------------------------------------------- audio ---

namespace audio {
// Aura's replies and the boot greeting are both MP3; the decoder reports each
// stream's real rate and the speaker is reconfigured to match.
constexpr uint32_t kMicrophoneSampleRate = 16000;
constexpr int32_t kGreetingVolumePercent = 30;
constexpr int32_t kSpeakingVolumePercent = 70;

constexpr int kDmaBufferCount = 6;
constexpr int kDmaBufferLength = 256;
// The DMA still holds kDmaBufferCount * kDmaBufferLength frames after the last
// i2s_write returns. Acknowledging before those samples reach the amplifier
// would unmute the microphone while Bunty is still audibly talking.
constexpr uint32_t kDmaDrainMs =
    (kDmaBufferCount * kDmaBufferLength * 1000) / kMicrophoneSampleRate + 40;

// Compressed-input working set for the Helix decoder. The greeting reads from
// flash and needs only enough to span one frame plus its bit reservoir; the
// gateway reply absorbs bursty WebSocket packets.
constexpr size_t kGreetingInputBytes = 4096;
constexpr size_t kReplyInputBytes = 8192;
}  // namespace audio

// ----------------------------------------------------------------- voice ---

namespace voice {
// VAD frame size, mode, endpoint and idle timeout live in VoiceActivity.h.
static_assert(audio::kMicrophoneSampleRate == kVadSampleRate,
              "VAD and capture sample rates must match");

// VAD starts the network connection. Keep several seconds in PSRAM so speech is
// not lost while the main loop finishes a dashboard TCP timeout and TLS.
constexpr size_t kCaptureRingBytes = 307200;   // ~9.6 s at 16 kHz.
// Aura can return a whole sentence faster than the speaker can play its decoded
// PCM. Keep the compressed MP3 burst in PSRAM instead of dropping it.
constexpr size_t kPlaybackRingBytes = 196608;
// A socket can open and then stall before session.ready if the gateway or the
// Flux leg never completes. Without a ceiling the client would wait forever.
constexpr uint32_t kConnectTimeoutMs = 15000;
// Close an application-idle turn even if the WebSocket heartbeat is healthy.
// Microphone PCM, gateway controls, and audible reply playback all refresh
// this timer; protocol ping/pong frames deliberately do not.
constexpr uint32_t kSocketIdleTimeoutMs = 20000;
constexpr uint32_t kStartRetryMs = 5000;
constexpr int kSocketBufferBytes = 4096;
constexpr int kSocketTaskStack = 6144;
constexpr int kSocketTaskPriority = 5;
constexpr int kPingIntervalSec = 10;
constexpr int kPingTimeoutSec = 25;

// Entering and leaving Bunty's face costs a full-screen clear plus a dashboard
// redraw. Holding it briefly stops a marginal detection from strobing between
// the two.
constexpr uint32_t kFaceMinimumMs = 1500;
constexpr uint32_t kMouthFrameMs = 50;
constexpr uint32_t kWaveformFrameMs = 45;
// One sample per redraw; sized to the number of bars that fit across the panel.
constexpr size_t kWaveformSamples = 58;
// This microphone's quiet floor is roughly 70 RMS and normal speech a few
// hundred; a higher ceiling made genuine speech look almost flat on screen.
constexpr uint32_t kWaveformFullScaleRms = 900;
constexpr uint32_t kHeartbeatMs = 3000;
}  // namespace voice

// --------------------------------------------------------------- network ---

namespace net {
// TLS certificate validity depends on a real clock.
constexpr time_t kMinimumTrustedUnixTime = 1704067200;  // 2024-01-01 UTC
constexpr uint32_t kClockSyncRetryMs = 30000;
constexpr uint32_t kMdnsQueryTimeoutMs = 3000;
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
constexpr char kWifiSsid[] = "wifi_ssid";
constexpr char kWifiPassword[] = "wifi_pass";
constexpr char kDashboard[] = "dashboard";
constexpr char kPaired[] = "paired";
constexpr char kBleAddress[] = "ble_addr";

constexpr char kIotToken[] = "iot_token";
constexpr char kIotDevice[] = "iot_device";
constexpr char kIotSession[] = "iot_session";

constexpr const char *kGaugeKeys[] = {
    kApiVersion, kDeviceId, kServerId,     kToken,     kServiceType,
    kPath,       kPort,     kWifiSsid,     kWifiPassword, kDashboard,
    kPaired,
};
}  // namespace nvs

}  // namespace cfg
