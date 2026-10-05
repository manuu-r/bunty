#pragma once
#include "Runtime.h"
#include <atomic>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <lwip/ip4_addr.h>
class IPAddress {
 public:
  IPAddress()=default;
  explicit IPAddress(uint32_t raw):raw_(raw) {}
  uint8_t operator[](size_t index) const { return reinterpret_cast<const uint8_t *>(&raw_)[index]; }
  bool fromString(const char *value) { return ip4addr_aton(value,reinterpret_cast<ip4_addr_t *>(&raw_)); }
  bool fromString(const String &value) { return fromString(value.c_str()); }
  bool operator==(const IPAddress &other) const { return raw_==other.raw_; }
  bool operator!=(const IPAddress &other) const { return !(*this==other); }
  uint32_t raw() const { return raw_; }
 private:
  uint32_t raw_=0;
};
enum wl_status_t { WL_DISCONNECTED=0, WL_CONNECTED=3, WL_NO_SHIELD=255 };
constexpr wifi_mode_t WIFI_STA=WIFI_MODE_STA;
using wifi_power_t=int8_t;
class NativeWiFi {
 public:
  bool mode(wifi_mode_t);
  void setAutoReconnect(bool value) { reconnect_=value; }
  bool begin(const char *ssid,const char *password);
  bool reconnect();
  void disconnect(bool powerOff=false,bool erase=false);
  wl_status_t status() const { return connected_?WL_CONNECTED:WL_DISCONNECTED; }
  bool setSleep(bool value);
  bool setTxPower(wifi_power_t value);
  int32_t RSSI() const;
  IPAddress localIP() const;
  IPAddress subnetMask() const;
  uint8_t failureReason() const { return reason_; }
 private:
  static void event(void *context,esp_event_base_t base,int32_t id,void *data);
  esp_netif_t *netif_=nullptr;
  bool initialized_=false,started_=false;
  std::atomic<bool> connected_{false},reconnect_{true},joining_{false};
  std::atomic<uint8_t> reason_{0};
};
inline NativeWiFi WiFi;
