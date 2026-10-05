#include "Log.h"
#include "Pairing.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cctype>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#include <esp_gatt_common_api.h>
#include <esp_random.h>
#include "Config.h"
#include "Memory.h"
#include "Ui.h"
#include "Touch.h"

namespace pairing {
namespace {
using namespace cfg::pairing;
Preferences *storage_ = nullptr;
String deviceName_, hardwareId_, error_, identity_;
std::atomic<bool> started_{false}, authenticated_{false}, configurationReady_{false};
std::atomic<bool> serviceReady_{false}, advReady_{false}, scanReady_{false}, advertised_{false};
std::atomic<bool> startupFailed_{false}, showPairing_{false};
std::atomic<int8_t> addressState_{-1};
std::atomic<int> pendingBonds_{0};
bool wifiJoinStarted_ = false;
bool memoryReleased_ = false;
uint32_t wifiJoinStartedAt_ = 0, completedAt_ = 0;
char configBuffer_[kMaximumConfigBytes + 1] = {};
size_t configLength_ = 0;
void *configJsonStorage_ = nullptr;
memory::Arena configArena_;
uint8_t expectedFrames_ = 0, nextFrame_ = 0;
std::atomic<uint32_t> comparisonPin_{UINT32_MAX};
esp_bd_addr_t comparisonPeer_ = {};
bool confirmationShown_ = false;
uint32_t confirmationDeadline_ = 0;
constexpr size_t kAttributeCount = 7;
uint16_t handles_[kAttributeCount] = {};
esp_gatts_attr_db_t attributes_[kAttributeCount] = {};
uint16_t primaryUuid_ = ESP_GATT_UUID_PRI_SERVICE;
uint16_t declarationUuid_ = ESP_GATT_UUID_CHAR_DECLARE;
uint8_t uuids_[4][16] = {};
uint8_t readProperty_ = ESP_GATT_CHAR_PROP_BIT_READ;
uint8_t writeProperty_ = ESP_GATT_CHAR_PROP_BIT_WRITE;
uint8_t emptyValue_ = 0;
uint8_t adv_[21] = {}, scan_[31] = {};
esp_ble_adv_params_t advParams_ = {};

void setStatus(const char *status) {
  if (handles_[6]) esp_ble_gatts_set_attr_value(handles_[6], strlen(status),
      reinterpret_cast<const uint8_t *>(status));
  logPrintf("[pairing] %s\n", status);
}
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

  IPAddress serverIp;
  const char *address = document["server_ipv4"] | "";
  if (*address && serverIp.fromString(address) && serverIp[0] != 0 && serverIp[0] != 127) {
    storage_->putString(cfg::nvs::kServerIp, address);
  } else {
    storage_->remove(cfg::nvs::kServerIp);
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

void reject(const char *status) {
    configLength_ = 0;
    expectedFrames_ = 0;
    nextFrame_ = 0;
    configurationReady_ = false;
    setStatus(status);
  }
void receiveFrame(const uint8_t *frame, size_t frameSize) {
    if (configurationReady_) return;  // main task owns the completed buffer
    if (!authenticated_) {
      reject("error:not authenticated");
      return;
    }
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
  String json;
  serializeJson(document, json);
  return json;
}


// Both identity reads and commissioning writes require encrypted MITM
// authentication. Numeric comparison is handled on the main task; BLE's
// callback task never waits for a touch or draws on the panel.
void parseUuid(const char *text, uint8_t *uuid) {
  int out = 15;
  for (size_t i = 0; text[i];) {
    if (text[i] == '-') { ++i; continue; }
    char pair[] = {text[i], text[i + 1], 0};
    uuid[out--] = strtoul(pair, nullptr, 16);
    i += 2;
  }
}
void attribute(int index, uint16_t uuidLength, uint8_t *uuid, uint16_t permissions,
               uint16_t capacity, uint16_t length, uint8_t *value) {
  auto &a = attributes_[index];
  a.attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
  a.att_desc.uuid_length = uuidLength;
  a.att_desc.uuid_p = uuid;
  a.att_desc.perm = permissions;
  a.att_desc.max_length = capacity;
  a.att_desc.length = length;
  a.att_desc.value = value;
}
void buildAttributes() {
  parseUuid(kServiceUuid, uuids_[0]);
  parseUuid(kIdentityCharacteristicUuid, uuids_[1]);
  parseUuid(kConfigCharacteristicUuid, uuids_[2]);
  parseUuid(kStatusCharacteristicUuid, uuids_[3]);
  identity_ = identityJson();
  attribute(0, 2, reinterpret_cast<uint8_t *>(&primaryUuid_), ESP_GATT_PERM_READ, 16, 16, uuids_[0]);
  for (int i = 0; i < 3; ++i) {
    attribute(1 + i * 2, 2, reinterpret_cast<uint8_t *>(&declarationUuid_), ESP_GATT_PERM_READ,
              1, 1, i == 1 ? &writeProperty_ : &readProperty_);
  }
  attribute(2, 16, uuids_[1], ESP_GATT_PERM_READ_ENC_MITM, 512, identity_.size(),
            reinterpret_cast<uint8_t *>(identity_.data()));
  attribute(4, 16, uuids_[2], ESP_GATT_PERM_WRITE_ENC_MITM, kMtu, 0, &emptyValue_);
  attributes_[4].attr_control.auto_rsp = ESP_GATT_RSP_BY_APP;
  attribute(6, 16, uuids_[3], ESP_GATT_PERM_READ_ENC_MITM, 256, 5,
            reinterpret_cast<uint8_t *>(const_cast<char *>("ready")));
}
void clearStoredBonds() {
  const int count = esp_ble_get_bond_device_num();
  if (count <= 0) return;
  auto *peers = static_cast<esp_ble_bond_dev_t *>(malloc(sizeof(esp_ble_bond_dev_t) * count));
  if (!peers) { startupFailed_ = true; return; }
  int found = count;
  if (esp_ble_get_bond_device_list(&found, peers) != ESP_OK) { free(peers); startupFailed_ = true; return; }
  // Removal completes asynchronously. Never advertise before the old keys
  // have actually been removed, otherwise a reconnect can skip comparison.
  pendingBonds_ += found;
  for (int i = 0; i < found; ++i) {
    if (esp_ble_remove_bond_device(peers[i].bd_addr) != ESP_OK) {
      --pendingBonds_; startupFailed_ = true;
    }
  }
  free(peers);
}
void advertiseWhenReady() {
  if (!started_ || startupFailed_ || addressState_ != 1 || !serviceReady_ ||
      !advReady_ || !scanReady_ || pendingBonds_ != 0 || advertised_) return;
  advertised_ = true;
  if (esp_ble_gap_start_advertising(&advParams_) != ESP_OK) startupFailed_ = true;
}
void gapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p) {
  switch (event) {
    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:
      addressState_ = p->set_rand_addr_cmpl.status == ESP_BT_STATUS_SUCCESS ? 1 : 0;
      if (addressState_ == 0) startupFailed_ = true;
      break;
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
      advReady_ = p->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS;
      if (!advReady_) startupFailed_ = true;
      break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
      scanReady_ = p->scan_rsp_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS;
      if (!scanReady_) startupFailed_ = true;
      break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
      if (p->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) startupFailed_ = true;
      break;
    case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
      --pendingBonds_;
      if (p->remove_bond_dev_cmpl.status != ESP_BT_STATUS_SUCCESS) startupFailed_ = true;
      break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
      esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_NC_REQ_EVT:
      memcpy(comparisonPeer_, p->ble_security.key_notif.bd_addr, sizeof(esp_bd_addr_t));
      comparisonPin_.store(p->ble_security.key_notif.passkey, std::memory_order_release);
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      const auto &auth = p->ble_security.auth_cmpl;
      authenticated_ = auth.success && (auth.auth_mode & ESP_LE_AUTH_REQ_MITM) &&
                       (auth.auth_mode & ESP_LE_AUTH_REQ_SC_ONLY);
      logPrintf("[pairing] authentication %s\n", authenticated_ ? "succeeded" : "failed");
      if (!authenticated_) { comparisonPin_ = UINT32_MAX; showPairing_ = true; }
      break;
    }
    default: break;
  }
  advertiseWhenReady();
}
void gattsEvent(esp_gatts_cb_event_t event, esp_gatt_if_t interface, esp_ble_gatts_cb_param_t *p) {
  switch (event) {
    case ESP_GATTS_REG_EVT:
      if (p->reg.status != ESP_GATT_OK ||
          esp_ble_gatts_create_attr_tab(attributes_, interface, kAttributeCount, 0) != ESP_OK)
        startupFailed_ = true;
      break;
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
      if (p->add_attr_tab.status != ESP_GATT_OK || p->add_attr_tab.num_handle != kAttributeCount) {
        startupFailed_ = true; break;
      }
      memcpy(handles_, p->add_attr_tab.handles, sizeof(handles_));
      if (esp_ble_gatts_start_service(handles_[0]) != ESP_OK) startupFailed_ = true;
      break;
    case ESP_GATTS_START_EVT:
      serviceReady_ = p->start.status == ESP_GATT_OK;
      if (!serviceReady_) startupFailed_ = true;
      break;
    case ESP_GATTS_CONNECT_EVT:
      logPrintf("[pairing] BLE connected\n");
      authenticated_ = false;
      esp_ble_set_encryption(p->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
      break;
    case ESP_GATTS_DISCONNECT_EVT:
      logPrintf("[pairing] BLE disconnected\n");
      authenticated_ = false; comparisonPin_ = UINT32_MAX; advertised_ = false;
      if (started_ && !configurationReady_ && completedAt_ == 0) {
        reject("ready"); clearStoredBonds(); showPairing_ = true;
      }
      break;
    case ESP_GATTS_WRITE_EVT:
      if (p->write.handle == handles_[4]) {
        const esp_gatt_status_t status = !authenticated_ ? ESP_GATT_INSUF_AUTHENTICATION :
            p->write.is_prep || p->write.offset ? ESP_GATT_REQ_NOT_SUPPORTED : ESP_GATT_OK;
        if (status == ESP_GATT_OK) receiveFrame(p->write.value, p->write.len);
        if (p->write.need_rsp) esp_ble_gatts_send_response(interface, p->write.conn_id,
            p->write.trans_id, status, nullptr);
      }
      break;
    default: break;
  }
  advertiseWhenReady();
}
void serviceConfirmation() {
  if (showPairing_.exchange(false)) { confirmationShown_ = false; ui::showPairing(); }
  const uint32_t pin = comparisonPin_.load(std::memory_order_acquire);
  if (pin == UINT32_MAX) { confirmationShown_ = false; return; }
  if (!confirmationShown_) {
    if (!touch::available()) {
      esp_ble_confirm_reply(comparisonPeer_, false); comparisonPin_ = UINT32_MAX; return;
    }
    ui::showNumericComparison(pin); touch::flush();
    confirmationDeadline_ = millis() + cfg::touch::kPairingDecisionTimeoutMs;
    confirmationShown_ = true;
  }
  const auto event = touch::poll();
  const bool accept = event.kind == touch::Event::Kind::kTap && ui::confirmAcceptedAt(event.x, event.y);
  const bool reject = event.kind == touch::Event::Kind::kTap && ui::confirmRejectedAt(event.x, event.y);
  if (!accept && !reject && static_cast<int32_t>(millis() - confirmationDeadline_) < 0) return;
  logPrintf("[pairing] local decision: %s\n", accept ? "accepted" : "rejected or timed out");
  esp_ble_confirm_reply(comparisonPeer_, accept);
  comparisonPin_ = UINT32_MAX; confirmationShown_ = false;
  if (accept) ui::showPairingConfirmed(); else ui::showPairing();
}
}  // namespace

void begin(Preferences &storage, const String &deviceName, const String &hardwareId) {
  storage_ = &storage; deviceName_ = deviceName; hardwareId_ = hardwareId;
}
const char *error() { return error_.c_str(); }
bool active() { return started_; }

void stop() {
  started_ = false;
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
    esp_ble_gap_stop_advertising(); esp_bluedroid_disable();
  }
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) esp_bluedroid_deinit();
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) esp_bt_controller_disable();
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) esp_bt_controller_deinit();
  heap_caps_free(configJsonStorage_); configJsonStorage_ = nullptr;
  memset(handles_, 0, sizeof(handles_));
  Serial.println("[pairing] Bluetooth stopped");
}
void releaseMemory() {
  if (memoryReleased_) return;
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE &&
      esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED &&
      esp_bt_mem_release(ESP_BT_MODE_BLE) == ESP_OK) {
    memoryReleased_ = true;
    Serial.println("[pairing] Bluetooth memory released");
  }
}
bool start() {
  if (memoryReleased_) { error_ = "Restart to pair"; return false; }
  if (started_) return true;
  configJsonStorage_ = ESP.getPsramSize() ? memory::external(8192) : memory::internal(8192);
  if (!configJsonStorage_) { error_ = "Setup memory"; return false; }
  configArena_.bind(configJsonStorage_, 8192);
  configurationReady_ = false; authenticated_ = false;
  serviceReady_ = advReady_ = scanReady_ = advertised_ = false;
  startupFailed_ = false; addressState_ = -1; pendingBonds_ = 0;
  wifiJoinStarted_ = false; completedAt_ = 0;
  buildAttributes();
  esp_bt_controller_config_t controller = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_bluedroid_config_t host = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
  auto fail = []() { error_ = "BLE initialization"; stop(); return false; };
  if (esp_bt_controller_init(&controller) != ESP_OK ||
      esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK ||
      esp_bluedroid_init_with_cfg(&host) != ESP_OK || esp_bluedroid_enable() != ESP_OK ||
      esp_ble_gatts_register_callback(gattsEvent) != ESP_OK ||
      esp_ble_gap_register_callback(gapEvent) != ESP_OK ||
      esp_ble_gap_set_device_name(deviceName_.c_str()) != ESP_OK ||
      esp_ble_gatt_set_local_mtu(kMtu) != ESP_OK) return fail();
  esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_MITM_BOND;
  esp_ble_io_cap_t capability = ESP_IO_CAP_IO;
  uint8_t size = 16, keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t onlySpecified = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
  if (esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth)) != ESP_OK ||
      esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &capability, sizeof(capability)) != ESP_OK ||
      esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &size, sizeof(size)) != ESP_OK ||
      esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys)) != ESP_OK ||
      esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys)) != ESP_OK ||
      esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &onlySpecified, sizeof(onlySpecified)) != ESP_OK)
    return fail();
  clearStoredBonds();
  esp_bd_addr_t address; loadAddress(address);
  // Flags plus complete 128-bit service UUID in advertising; full name in
  // scan response. Both packets fit the legacy 31-byte limit.
  adv_[0] = 2; adv_[1] = ESP_BLE_AD_TYPE_FLAG; adv_[2] = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
  adv_[3] = 17; adv_[4] = ESP_BLE_AD_TYPE_128SRV_CMPL; memcpy(adv_ + 5, uuids_[0], 16);
  const size_t nameLength = std::min(deviceName_.size(), size_t(29));
  scan_[0] = nameLength + 1; scan_[1] = ESP_BLE_AD_TYPE_NAME_CMPL;
  memcpy(scan_ + 2, deviceName_.data(), nameLength);
  advParams_.adv_int_min = 0x20; advParams_.adv_int_max = 0x40;
  advParams_.adv_type = ADV_TYPE_IND; advParams_.own_addr_type = BLE_ADDR_TYPE_RANDOM;
  advParams_.channel_map = ADV_CHNL_ALL; advParams_.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
  started_ = true;
  if (esp_ble_gap_set_rand_addr(address) != ESP_OK ||
      esp_ble_gap_config_adv_data_raw(adv_, sizeof(adv_)) != ESP_OK ||
      esp_ble_gap_config_scan_rsp_data_raw(scan_, nameLength + 2) != ESP_OK ||
      esp_ble_gatts_app_register(0) != ESP_OK) return fail();
  const uint32_t deadline = millis() + 5000;
  while (!advertised_ && !startupFailed_ && static_cast<int32_t>(millis() - deadline) < 0) delay(5);
  if (!advertised_ || startupFailed_) return fail();
  logPrintf("[pairing] advertising as %s\n", deviceName_.c_str());
  return true;
}

bool service() {
  serviceConfirmation();
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
      const uint8_t reason = WiFi.failureReason();
      if (reason == WIFI_REASON_NO_AP_FOUND) {
        setStatus("error:Wi-Fi network not found. Choose a 2.4 GHz network, check its name, and move Bunty closer to the router.");
      } else if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
        setStatus("error:Wi-Fi authentication failed. Check the Wi-Fi password and router security settings.");
      } else {
        setStatus("error:Wi-Fi connection timed out. Check the router and retry on a 2.4 GHz network.");
      }
      configurationReady_ = false;
      wifiJoinStarted_ = false;
      WiFi.disconnect(false, false);
      ui::showPairing();
    }
  }

  if (completedAt_ != 0 && millis() - completedAt_ > kShutdownDelayMs) {
    // Bluedroid owns a large portion of internal heap; release it once done.
    stop();
    return true;
  }

  ui::servicePairingActivity();
  return false;
}

}  // namespace pairing
