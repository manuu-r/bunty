#include <Preferences.h>
#include <cassert>
#include <iostream>
#include <map>
#include <vector>

struct Value { nvs_type_t type; std::vector<uint8_t> bytes; };
std::map<std::string, Value> values;
bool commitFails=false;
esp_err_t nvs_open(const char *,nvs_open_mode_t,nvs_handle_t *h) { *h=1; return ESP_OK; }
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_commit(nvs_handle_t) { return commitFails?ESP_FAIL:ESP_OK; }
esp_err_t nvs_find_key(nvs_handle_t,const char *key,nvs_type_t *type) {
  auto it=values.find(key); if(it==values.end()) return ESP_FAIL; *type=it->second.type; return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t,const char *key) { return values.erase(key)?ESP_OK:ESP_FAIL; }
esp_err_t save(const char *key,nvs_type_t type,const void *data,size_t length) {
  const auto *bytes=static_cast<const uint8_t *>(data);
  values[key]={type,{bytes,bytes+length}}; return ESP_OK;
}
esp_err_t load(const char *key,nvs_type_t type,void *data,size_t *length) {
  auto it=values.find(key); if(it==values.end() || it->second.type!=type) return ESP_FAIL;
  const size_t size=it->second.bytes.size();
  if(data && *length<size) return ESP_FAIL;
  if(data) std::memcpy(data,it->second.bytes.data(),size);
  *length=size; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t,const char *k,const char *v) { return save(k,NVS_TYPE_STR,v,std::strlen(v)+1); }
esp_err_t nvs_get_str(nvs_handle_t,const char *k,char *v,size_t *n) { return load(k,NVS_TYPE_STR,v,n); }
esp_err_t nvs_set_blob(nvs_handle_t,const char *k,const void *v,size_t n) { return save(k,NVS_TYPE_BLOB,v,n); }
esp_err_t nvs_get_blob(nvs_handle_t,const char *k,void *v,size_t *n) { return load(k,NVS_TYPE_BLOB,v,n); }
#define SCALAR(bits) \
  esp_err_t nvs_set_u##bits(nvs_handle_t,const char *k,uint##bits##_t v) { return save(k,NVS_TYPE_U##bits,&v,sizeof(v)); } \
  esp_err_t nvs_get_u##bits(nvs_handle_t,const char *k,uint##bits##_t *v) { size_t n=sizeof(*v); return load(k,NVS_TYPE_U##bits,v,&n); }
SCALAR(8)
SCALAR(16)
SCALAR(32)
int main() {
  // Seed precisely the on-flash representations written by Arduino Preferences.
  nvs_set_str(1,"wifi_ssid","existing-network");
  nvs_set_str(1,"iot_token","existing-credential");
  nvs_set_u8(1,"paired",1);
  nvs_set_u16(1,"port",12345);
  nvs_set_u32(1,"fuel_mas",987654);
  const float speed=30.5f;
  nvs_set_blob(1,"turn-speed",&speed,sizeof(speed));
  const uint8_t address[]={0xc1,2,3,4,5,6};
  nvs_set_blob(1,"ble_addr",address,sizeof(address));
  Preferences storage;
  assert(storage.begin("bunty"));
  assert(storage.getString("wifi_ssid")=="existing-network");
  assert(storage.getBool("paired"));
  assert(storage.getUShort("port")==12345);
  assert(storage.getUInt("fuel_mas")==987654);
  assert(storage.getFloat("turn-speed")==speed);
  uint8_t readAddress[6]={};
  assert(storage.getBytesLength("ble_addr")==6);
  assert(storage.getBytes("ble_addr",readAddress,sizeof(readAddress))==6);
  assert(std::memcmp(address,readAddress,6)==0);
  char token[64]={};
  assert(storage.getString("iot_token",token,sizeof(token))==19);
  assert(std::strcmp(token,"existing-credential")==0);
  char small[2]={'x','x'};
  assert(storage.getString("iot_token",small,sizeof(small))==0 && small[0]==0);
  assert(storage.getString("missing","fallback")=="fallback");
  assert(storage.getUShort("missing",42)==42);
  assert(storage.getFloat("missing",1.5f)==1.5f);
  assert(storage.putBool("paired",false)==1 && values.at("paired").type==NVS_TYPE_U8);
  assert(storage.putFloat("turn-speed",20.0f)==4 && values.at("turn-speed").type==NVS_TYPE_BLOB);
  assert(storage.putString("wifi_pass","")==0 && storage.isKey("wifi_pass"));
  assert(storage.getString("wifi_pass")=="");
  commitFails=true;
  assert(storage.putUInt("fuel_mas",12)==0);
  std::cout << "Native storage: legacy NVS strings/scalars/blobs, empty passwords and failure handling passed\n";
}
