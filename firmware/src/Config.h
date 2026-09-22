#pragma once

#include <driver/i2s.h>
#include <stdint.h>

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
// USB. Nothing drives audio yet; SD is held low so the amp stays shut down.
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
// Temporary bench check: at boot, sweep yaw then pitch fast, then home.
constexpr bool kBootDemo = false;
// Head gestures between dashboard pages.
constexpr bool kPageSweep = true;
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
// Bunty looks this far to each side of home before a new dashboard page.
constexpr float kSweepDegrees = 40.0f;
// A full 100 -> 140 -> 60 -> 100 sweep is 160 degrees of travel. A conservative
// rate keeps the 50 Hz position changes below half a degree, avoiding visible
// stepping in small hobby servos while still completing inside the page dwell.
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
constexpr uint16_t kBatteryMilliampHours = 1500;

// The policy falls out of this arithmetic rather than out of special cases:
// exactly one servo runs at a time.
static_assert(kBaseMilliamps + kBacklightMilliamps + kServoMilliamps <=
                  kRailMilliamps - kReserveMilliamps,
              "one servo must fit inside the rail budget");
static_assert(kBaseMilliamps + kBacklightMilliamps + 2 * kServoMilliamps >
                  kRailMilliamps - kReserveMilliamps,
              "two servos at once must not fit inside the rail budget");

// Quarter-dBm; 52 is 13 dBm, roughly halving the TX burst the reserve covers.
constexpr int8_t kWifiTxPowerQuarterDbm = 52;

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

// --------------------------------------------------------------- network ---

namespace net {
constexpr uint32_t kMdnsQueryTimeoutMs = 3000;
}  // namespace net

// ------------------------------------------------------- persisted state ---

// NVS namespace and keys. Unpairing clears kGaugeKeys only; the Bluetooth
// address is minted once and survives re-pairing, so this accessory keeps one identity for its whole life.
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

constexpr const char *kGaugeKeys[] = {
    kApiVersion, kDeviceId, kServerId,     kToken,     kServiceType,
    kPath,       kPort, kServerIp,     kWifiSsid,     kWifiPassword, kDashboard,
    kPaired,
};
}  // namespace nvs

}  // namespace cfg
