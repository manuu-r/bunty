#pragma once
#include <cstddef>
#include <cstdint>
using nvs_handle_t = uint32_t;
using esp_err_t = int;
constexpr esp_err_t ESP_OK=0, ESP_FAIL=-1;
enum nvs_type_t { NVS_TYPE_U8, NVS_TYPE_U16, NVS_TYPE_U32, NVS_TYPE_STR, NVS_TYPE_BLOB };
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_find_key(nvs_handle_t, const char *, nvs_type_t *);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_get_u16(nvs_handle_t, const char *, uint16_t *);
esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_set_u16(nvs_handle_t, const char *, uint16_t);
esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t);
