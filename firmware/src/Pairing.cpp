#include "Log.h"
#include "Pairing.h"

#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cctype>
#include <esp_gap_ble_api.h>

#include "Config.h"
#include "Memory.h"
#include "Ui.h"
#include "TapInput.h"

namespace pairing {
namespace {

using namespace cfg::pairing;

Preferences *storage_ = nullptr;
TapInput *taps_ = nullptr;
String deviceName_;
String hardwareId_;
String error_;

BLEServer *server_ = nullptr;
BLECharacteristic *status_ = nullptr;
BLESecurity security_;

bool started_ = false;
bool wifiJoinStarted_ = false;
uint32_t wifiJoinStartedAt_ = 0;
uint32_t completedAt_ = 0;

// Written from the Bluedroid callback task, read from loop().
std::atomic<bool> configurationReady_{false};
std::atomic<bool> authenticated_{false};
// -1 while the controller is applying the static random address, 0 on failure,
// and 1 when it is safe to advertise with that address.
std::atomic<int8_t> addressState_{-1};

char configBuffer_[kMaximumConfigBytes + 1] = {};
size_t configLength_ = 0;
void *configJsonStorage_ = nullptr;
memory::Arena configArena_;
uint8_t expectedFrames_ = 0;
uint8_t nextFrame_ = 0;

void setStatus(const char *status) {
  if (status_) status_->setValue(status);
  logPrintf("[pairing] %s\n", status);
}

// ------------------------------------------------------------- validation ---

// Mirrors device_identifier() in src/provisioning.rs. Gauge reads this stable
// hardware-derived value only after BLE Secure Connections authentication and
// sends it back inside the protected commissioning document.
bool isIdentifier(const char *value) {
  const size_t length = strlen(value);
  if (length == 0 || length > 64) return false;
  for (size_t i = 0; i < length; ++i) {
    const char ch = value[i];
    if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' &&
        ch != '_' && ch != '.') {
      return false;
    }
  }
  return true;
}

bool isHexToken(const char *token) {
  if (strlen(token) != 64) return false;
  for (size_t i = 0; i < 64; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(token[i]))) return false;
  }
  return true;
}

// A WPA passphrase is 8-63 characters; exactly 64 means a raw hex PSK.
bool validWifiPassword(const char *password) {
  const size_t length = strlen(password);
  if (length < 64) return true;
  if (length != 64) return false;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(password[i]))) return false;
  }
  return true;
}

bool saveCommission(JsonDocument &document) {
  JsonObjectConst wifi = document["wifi"].as<JsonObjectConst>();
  JsonObjectConst accessory = document["accessory"].as<JsonObjectConst>();
  const char *ssid = wifi["ssid"] | "";
  const char *password = wifi["password"] | "";
  const char *configuredDevice = accessory["device_id"] | "";
  const char *serverId = accessory["server_id"] | "";
  const char *token = accessory["bearer_token"] | "";
  const char *service = accessory["service_type"] | "";
  const char *path = accessory["dashboard_path"] | "";
  const uint16_t version = accessory["version"] | 0;
  const uint16_t port = accessory["server_port"] | 0;

  if (strcmp(document["protocol"] | "", kCommissionProtocol) != 0 ||
      static_cast<uint16_t>(document["version"] | 0) != 1 ||
      strlen(ssid) == 0 || strlen(ssid) > 32 || !validWifiPassword(password) ||
      strcmp(accessory["protocol"] | "", kAccessoryProtocol) != 0 ||
      version != 1 || hardwareId_ != configuredDevice ||
      !isIdentifier(configuredDevice) || strlen(serverId) == 0 ||
      strlen(serverId) > 64 || !isHexToken(token) ||
      strcmp(service, cfg::dashboard::kServiceType) != 0 ||
      strcmp(path, cfg::dashboard::kDefaultPath) != 0 || port == 0) {
    return false;
  }

  return storage_->putUShort(cfg::nvs::kApiVersion, version) ==
             sizeof(uint16_t) &&
         storage_->putString(cfg::nvs::kDeviceId, configuredDevice) > 0 &&
         storage_->putString(cfg::nvs::kServerId, serverId) > 0 &&
         storage_->putString(cfg::nvs::kToken, token) > 0 &&
         storage_->putString(cfg::nvs::kServiceType, service) > 0 &&
         storage_->putString(cfg::nvs::kPath, path) > 0 &&
         storage_->putUShort(cfg::nvs::kPort, port) == sizeof(uint16_t) &&
         storage_->putString(cfg::nvs::kWifiSsid, ssid) > 0 &&
         storage_->putString(cfg::nvs::kWifiPassword, password) ==
             strlen(password);
}

// -------------------------------------------------------------- callbacks ---

class ConfigCallbacks final : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic *characteristic) override {
    if (configurationReady_) return;  // main task owns the completed buffer
    if (!authenticated_) {
      reject("error:not authenticated");
      return;
    }
    const uint8_t *frame = characteristic->getData();
    const size_t frameSize = characteristic->getLength();
    if (frameSize < kFrameHeaderSize ||
        static_cast<uint8_t>(frame[0]) != kFrameMagic) {
      reject("error:bad setup frame");
      return;
    }
    const uint8_t sequence = static_cast<uint8_t>(frame[1]);
    const uint8_t total = static_cast<uint8_t>(frame[2]);
    if (total == 0) {
      reject("error:bad frame count");
      return;
    }
    if (sequence == 0) {
      configLength_ = 0;
      expectedFrames_ = total;
      nextFrame_ = 0;
      configurationReady_ = false;
      setStatus("receiving");
    }
    if (expectedFrames_ != total || sequence != nextFrame_ ||
        configLength_ + frameSize - kFrameHeaderSize >
            kMaximumConfigBytes) {
      reject("error:frame order");
      return;
    }
    memcpy(configBuffer_ + configLength_, frame + kFrameHeaderSize, frameSize - kFrameHeaderSize);
    configLength_ += frameSize - kFrameHeaderSize;
    configBuffer_[configLength_] = 0;
    ++nextFrame_;
    if (nextFrame_ != expectedFrames_) return;

    expectedFrames_ = 0;
    nextFrame_ = 0;
    setStatus("received");
    configurationReady_ = true;
  }

 private:
  void reject(const char *status) {
    configLength_ = 0;
    expectedFrames_ = 0;
    nextFrame_ = 0;
    configurationReady_ = false;
    setStatus(status);
  }
};

class SecurityCallbacks final : public BLESecurityCallbacks {
 public:
  uint32_t onPassKeyRequest() override { return 0; }

  void onPassKeyNotify(uint32_t passKey) override {
    logPrintf("[pairing] passkey %06lu\n",
                  static_cast<unsigned long>(passKey));
  }

  bool onSecurityRequest() override { return true; }

  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    authenticated_ = result.success;
    if (!result.success) {
      logPrintf("[pairing] authentication failed: 0x%02x\n",
                    result.fail_reason);
      ui::showPairing();
    }
  }

  bool onConfirmPIN(uint32_t pin) override {
    ui::showNumericComparison(pin);
    const bool accepted =
        taps_ && taps_->waitForPairingDecision() == TapDecision::Confirm;
    if (accepted) {
      ui::showPairingConfirmed();
    } else {
      ui::showPairing();
    }
    return accepted;
  }
};

class ServerCallbacks final : public BLEServerCallbacks {
 public:
  void onDisconnect(BLEServer *) override {
    authenticated_ = false;
    // Nothing is retained from an interrupted attempt: the session was never
    // bonded, so the next one starts from numeric comparison again.
    if (started_ && completedAt_ == 0) {
      BLEDevice::startAdvertising();
      ui::showPairing();
    }
  }
};

ConfigCallbacks configCallbacks;
SecurityCallbacks securityCallbacks;
ServerCallbacks serverCallbacks;

// --------------------------------------------------------------- identity ---

// Commissioning never bonds, so a stored link key can only be residue from an
// older firmware. Dropping it keeps the controller's view and the Mac's view of
// this accessory in agreement.
void clearStoredBonds() {
  const int count = esp_ble_get_bond_device_num();
  if (count <= 0) return;
  auto *peers = static_cast<esp_ble_bond_dev_t *>(
      malloc(sizeof(esp_ble_bond_dev_t) * count));
  int found = count;
  if (peers && esp_ble_get_bond_device_list(&found, peers) == ESP_OK) {
    for (int i = 0; i < found; ++i) {
      esp_ble_remove_bond_device(peers[i].bd_addr);
    }
  }
  free(peers);
}

// Minted once and kept forever. Rotating it made macOS file every pairing
// attempt as a separate accessory, so the Bluetooth list filled with dead
// entries that could never reconnect.
void loadAddress(esp_bd_addr_t address) {
  if (storage_->getBytesLength(cfg::nvs::kBleAddress) == sizeof(esp_bd_addr_t)) {
    storage_->getBytes(cfg::nvs::kBleAddress, address, sizeof(esp_bd_addr_t));
  } else {
    esp_fill_random(address, sizeof(esp_bd_addr_t));
  }
  // esp_bd_addr_t stores the most-significant octet first. Static random
  // addresses require address bits 47:46 to be 0b11; setting address[5] instead
  // made Bluedroid reject the value and silently prevented adverts.
  address[0] = static_cast<uint8_t>((address[0] & 0x3f) | 0xc0);
  storage_->putBytes(cfg::nvs::kBleAddress, address, sizeof(esp_bd_addr_t));
}

void gapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *parameter) {
  if (event != ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT) return;
  addressState_ =
      parameter->set_rand_addr_cmpl.status == ESP_BT_STATUS_SUCCESS ? 1 : 0;
}

String identityJson() {
  JsonDocument document;
  document["protocol"] = kPairingProtocol;
  document["version"] = kProtocolVersion;
  document["device_id"] = hardwareId_;
  document["name"] = deviceName_;
  document["kind"] = "display";
  document["firmware_version"] = kFirmwareVersion;
  document["capabilities"][0] = "dashboard.pull";
  document["capabilities"][1] = "dashboard.cache";
  document["capabilities"][2] = "display";
  document["capabilities"][3] = "audio.output";
  String json;
  serializeJson(document, json);
  return json;
}

}  // namespace

void begin(Preferences &storage, TapInput &taps, const String &deviceName,
           const String &hardwareId) {
  storage_ = &storage;
  taps_ = &taps;
  deviceName_ = deviceName;
  hardwareId_ = hardwareId;
}

const char *error() { return error_.c_str(); }
bool active() { return started_; }

bool start() {
  if (!configJsonStorage_) {
    configJsonStorage_ = ESP.getPsramSize() ? memory::external(8192) : memory::internal(8192);
    if (!configJsonStorage_) { error_ = "Setup memory"; return false; }
    configArena_.bind(configJsonStorage_, 8192);
  }
  BLEDevice::init(deviceName_.c_str());
  BLEDevice::setMTU(kMtu);
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
  BLEDevice::setSecurityCallbacks(&securityCallbacks);
  BLEDevice::setCustomGapHandler(gapEvent);
  // Secure Connections with numeric comparison, but deliberately no bond.
  // Commissioning is a single act that hands over Wi-Fi and a bearer token;
  // everything afterwards runs over the LAN, so a stored link key would only
  // leave a permanent, unusable entry in the Mac's Bluetooth list.
  security_.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM);
  security_.setCapability(ESP_IO_CAP_IO);
  security_.setKeySize(16);
  security_.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security_.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  clearStoredBonds();

  server_ = BLEDevice::createServer();
  if (!server_) {
    error_ = "BLE server";
    return false;
  }
  server_->setCallbacks(&serverCallbacks);

  BLEService *service = server_->createService(kServiceUuid);
  BLECharacteristic *identity = service->createCharacteristic(
      kIdentityCharacteristicUuid, BLECharacteristic::PROPERTY_READ);
  identity->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  identity->setValue(identityJson().c_str());

  BLECharacteristic *configuration = service->createCharacteristic(
      kConfigCharacteristicUuid, BLECharacteristic::PROPERTY_WRITE);
  configuration->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  configuration->setCallbacks(&configCallbacks);

  status_ = service->createCharacteristic(kStatusCharacteristicUuid,
                                          BLECharacteristic::PROPERTY_READ);
  status_->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  status_->setValue("ready");
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  esp_bd_addr_t address;
  loadAddress(address);
  addressState_ = -1;
  advertising->setDeviceAddress(address, BLE_ADDR_TYPE_RANDOM);
  const uint32_t deadline = millis() + 1500;
  while (addressState_ < 0 && static_cast<int32_t>(millis() - deadline) < 0) {
    delay(5);
  }
  if (addressState_ != 1) {
    error_ = "BLE address";
    Serial.println("[pairing] static random address was rejected");
    return false;
  }
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  started_ = true;
  logPrintf("[pairing] advertising as %s\n", deviceName_.c_str());
  return true;
}

void stop() {
  if (!started_) return;
  BLEDevice::stopAdvertising();
  BLEDevice::deinit(true);
  heap_caps_free(configJsonStorage_);
  configJsonStorage_ = nullptr;
  server_ = nullptr;
  status_ = nullptr;
  started_ = false;
  Serial.println("[pairing] Bluetooth stopped");
}

bool service() {
  if (configurationReady_ && !wifiJoinStarted_) {
    configArena_.reset();
    JsonDocument document(&configArena_);
    if (deserializeJson(document, configBuffer_, configLength_) || !saveCommission(document)) {
      setStatus("error:invalid setup");
      configurationReady_ = false;
      return false;
    }
    setStatus("joining");
    wifiJoinStarted_ = true;
    wifiJoinStartedAt_ = millis();
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    // ESP32-S3 requires Wi-Fi modem sleep while the Bluedroid controller is
    // still active. Disabling it here aborts inside ESP-IDF's coexistence
    // power-management path; the runtime disables sleep after stop() has fully
    // released Bluetooth.
    WiFi.setSleep(true);
    WiFi.begin(storage_->getString(cfg::nvs::kWifiSsid, "").c_str(),
               storage_->getString(cfg::nvs::kWifiPassword, "").c_str());
  }

  if (wifiJoinStarted_ && completedAt_ == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      storage_->putBool(cfg::nvs::kPaired, true);
      setStatus("connected");
      completedAt_ = millis();
    } else if (millis() - wifiJoinStartedAt_ > kWifiJoinTimeoutMs) {
      setStatus("error:wifi connection failed");
      configurationReady_ = false;
      wifiJoinStarted_ = false;
      WiFi.disconnect(false, false);
      ui::showPairing();
    }
  }

  if (completedAt_ != 0 && millis() - completedAt_ > kShutdownDelayMs) {
    // Bluedroid owns a large portion of internal heap. Release it before the
    // I2S driver allocates DMA memory for the greeting.
    stop();
    return true;
  }

  ui::servicePairingActivity();
  return false;
}

}  // namespace pairing
