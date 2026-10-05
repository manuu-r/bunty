#pragma once
#include <string>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <nvs.h>
// Preserve Arduino Preferences' NVS types/keys for existing flashed devices.
class Preferences {
 public:
  ~Preferences() { end(); }
  bool begin(const char *name,bool readOnly=false);
  void end();
  bool isKey(const char *key) const;
  bool remove(const char *key);
  std::string getString(const char *key,const char *fallback="") const;
  size_t getString(const char *key,char *buffer,size_t capacity) const;
  size_t putString(const char *key,const char *value);
  size_t getBytesLength(const char *key) const;
  size_t getBytes(const char *key,void *buffer,size_t capacity) const;
  size_t putBytes(const char *key,const void *buffer,size_t length);
  uint8_t getUChar(const char *key,uint8_t fallback=0) const;
  uint16_t getUShort(const char *key,uint16_t fallback=0) const;
  uint32_t getUInt(const char *key,uint32_t fallback=0) const;
  bool getBool(const char *key,bool fallback=false) const { return getUChar(key,fallback)!=0; }
  float getFloat(const char *key,float fallback=0) const;
  size_t putUChar(const char *key,uint8_t value);
  size_t putUShort(const char *key,uint16_t value);
  size_t putUInt(const char *key,uint32_t value);
  size_t putBool(const char *key,bool value) { return putUChar(key,value); }
  size_t putFloat(const char *key,float value) { return putBytes(key,&value,sizeof(value)); }
 private:
  nvs_handle_t handle_=0;
  size_t commit(esp_err_t error,size_t length);
};
