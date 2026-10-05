#include "Preferences.h"
bool Preferences::begin(const char *name,bool readOnly) {
  end(); return nvs_open(name,readOnly?NVS_READONLY:NVS_READWRITE,&handle_)==ESP_OK;
}
void Preferences::end() { if(handle_) nvs_close(handle_); handle_=0; }
size_t Preferences::commit(esp_err_t error,size_t length) {
  return error==ESP_OK && nvs_commit(handle_)==ESP_OK?length:0;
}
bool Preferences::isKey(const char *key) const {
  nvs_type_t type; return handle_ && nvs_find_key(handle_,key,&type)==ESP_OK;
}
bool Preferences::remove(const char *key) { return nvs_erase_key(handle_,key)==ESP_OK && nvs_commit(handle_)==ESP_OK; }
std::string Preferences::getString(const char *key,const char *fallback) const {
  size_t length=0; if(nvs_get_str(handle_,key,nullptr,&length)!=ESP_OK || !length) return fallback;
  std::string value(length,'\0');
  if(nvs_get_str(handle_,key,value.data(),&length)!=ESP_OK) return fallback;
  value.resize(length-1); return value;
}
size_t Preferences::getString(const char *key,char *buffer,size_t capacity) const {
  if(!capacity) return 0;
  buffer[0]=0; size_t length=capacity;
  return nvs_get_str(handle_,key,buffer,&length)==ESP_OK?length-1:0;
}
size_t Preferences::putString(const char *key,const char *value) { return commit(nvs_set_str(handle_,key,value),strlen(value)); }
size_t Preferences::getBytesLength(const char *key) const {
  size_t size=0; return nvs_get_blob(handle_,key,nullptr,&size)==ESP_OK?size:0;
}
size_t Preferences::getBytes(const char *key,void *buffer,size_t capacity) const {
  return nvs_get_blob(handle_,key,buffer,&capacity)==ESP_OK?capacity:0;
}
size_t Preferences::putBytes(const char *key,const void *buffer,size_t length) { return commit(nvs_set_blob(handle_,key,buffer,length),length); }
uint8_t Preferences::getUChar(const char *key,uint8_t fallback) const { uint8_t v; return nvs_get_u8(handle_,key,&v)==ESP_OK?v:fallback; }
uint16_t Preferences::getUShort(const char *key,uint16_t fallback) const { uint16_t v; return nvs_get_u16(handle_,key,&v)==ESP_OK?v:fallback; }
uint32_t Preferences::getUInt(const char *key,uint32_t fallback) const { uint32_t v; return nvs_get_u32(handle_,key,&v)==ESP_OK?v:fallback; }
float Preferences::getFloat(const char *key,float fallback) const { float v; return getBytes(key,&v,sizeof(v))==sizeof(v)?v:fallback; }
size_t Preferences::putUChar(const char *key,uint8_t value) { return commit(nvs_set_u8(handle_,key,value),1); }
size_t Preferences::putUShort(const char *key,uint16_t value) { return commit(nvs_set_u16(handle_,key,value),2); }
size_t Preferences::putUInt(const char *key,uint32_t value) { return commit(nvs_set_u32(handle_,key,value),4); }
