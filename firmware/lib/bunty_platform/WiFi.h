#pragma once
#include "Runtime.h"
#include <esp_wifi.h>

enum wl_status_t { WL_DISCONNECTED=0, WL_CONNECTED=3, WL_NO_SHIELD=255 };
using wifi_power_t=int8_t;
// Muse is the sole Wi-Fi driver owner. This facade only reads status and applies
// power settings; it cannot create a second netif, start, or reconnect a driver.
class NativeWiFi {
 public:
  wl_status_t status() const;
  bool setSleep(bool value);
  bool setTxPower(wifi_power_t value);
  int32_t RSSI() const;
};
inline NativeWiFi WiFi;
