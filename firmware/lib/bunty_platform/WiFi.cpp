#include "WiFi.h"
#include <esp_netif.h>

bool NativeWiFi::setSleep(bool value) {
  return esp_wifi_set_ps(value?WIFI_PS_MIN_MODEM:WIFI_PS_NONE)==ESP_OK;
}
bool NativeWiFi::setTxPower(wifi_power_t value) {
  return esp_wifi_set_max_tx_power(value)==ESP_OK;
}
int32_t NativeWiFi::RSSI() const {
  if(status()!=WL_CONNECTED) return 0;
  wifi_ap_record_t record={};
  return esp_wifi_sta_get_ap_info(&record)==ESP_OK?record.rssi:0;
}
wl_status_t NativeWiFi::status() const {
  esp_netif_ip_info_t ip={};
  auto *netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if(!netif || !esp_netif_is_netif_up(netif) ||
     esp_netif_get_ip_info(netif,&ip)!=ESP_OK || !ip.ip.addr) return WL_DISCONNECTED;
  wifi_ap_record_t ap={};
  return esp_wifi_sta_get_ap_info(&ap)==ESP_OK?WL_CONNECTED:WL_DISCONNECTED;
}
