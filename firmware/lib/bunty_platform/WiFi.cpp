#include "WiFi.h"
#include <esp_event.h>
bool NativeWiFi::mode(wifi_mode_t mode) {
  if(!initialized_) {
    netif_=esp_netif_create_default_wifi_sta();
    if(!netif_) return false;
    wifi_init_config_t c=WIFI_INIT_CONFIG_DEFAULT();
    if(esp_wifi_init(&c)!=ESP_OK) return false;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,event,this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,event,this));
    initialized_=true;
  }
  return esp_wifi_set_mode(mode)==ESP_OK;
}
bool NativeWiFi::begin(const char *ssid,const char *password) {
  if(!mode(WIFI_MODE_STA)) return false;
  joining_=false;
  if(started_) esp_wifi_disconnect();
  connected_=false; reason_=0;
  wifi_config_t c={};
  memcpy(c.sta.ssid,ssid,std::min(strlen(ssid),sizeof(c.sta.ssid)));
  // A 64-character hex PSK fills the IDF password field with no terminator.
  memcpy(c.sta.password,password,std::min(strlen(password),sizeof(c.sta.password)));
  if(esp_wifi_set_config(WIFI_IF_STA,&c)!=ESP_OK) return false;
  if(!started_) { if(esp_wifi_start()!=ESP_OK) return false; started_=true; }
  joining_=true;
  return esp_wifi_connect()==ESP_OK;
}
bool NativeWiFi::reconnect() {
  if(!started_) return false;
  joining_=false; esp_wifi_disconnect(); joining_=true;
  return esp_wifi_connect()==ESP_OK;
}
void NativeWiFi::disconnect(bool powerOff,bool erase) {
  joining_=false; connected_=false;
  if(!initialized_) return;
  esp_wifi_disconnect();
  if(erase) { wifi_config_t c={}; esp_wifi_set_config(WIFI_IF_STA,&c); }
  if(powerOff && started_) { esp_wifi_stop(); started_=false; }
}
bool NativeWiFi::setSleep(bool value) { return initialized_ && esp_wifi_set_ps(value?WIFI_PS_MIN_MODEM:WIFI_PS_NONE)==ESP_OK; }
bool NativeWiFi::setTxPower(wifi_power_t value) { return esp_wifi_set_max_tx_power(value)==ESP_OK; }
int32_t NativeWiFi::RSSI() const { wifi_ap_record_t r={}; return esp_wifi_sta_get_ap_info(&r)==ESP_OK?r.rssi:0; }
IPAddress NativeWiFi::localIP() const { esp_netif_ip_info_t i={}; if(netif_) esp_netif_get_ip_info(netif_,&i); return IPAddress(i.ip.addr); }
IPAddress NativeWiFi::subnetMask() const { esp_netif_ip_info_t i={}; if(netif_) esp_netif_get_ip_info(netif_,&i); return IPAddress(i.netmask.addr); }
void NativeWiFi::event(void *context,esp_event_base_t base,int32_t id,void *data) {
  auto *self=static_cast<NativeWiFi *>(context);
  if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) { self->connected_=true; self->reason_=0; }
  if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
    self->connected_=false; self->reason_=static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
    if(self->joining_ && self->reconnect_) esp_wifi_connect();
  }
}
