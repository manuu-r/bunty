#pragma once
#include <ArduinoJson.h>
#include <cstdint>

namespace dashboard_cache {
// A failed/initial provider refresh must not erase the last usable quotas.
inline bool preserveQuota(JsonDocument &incoming, const JsonDocument &previous) {
  if (!incoming["quota"]["providers"].as<JsonArrayConst>().isNull() &&
      incoming["quota"]["providers"].as<JsonArrayConst>().size() == 0 &&
      incoming["quota"]["errors"].as<JsonArrayConst>().size() != 0 &&
      previous["quota"]["providers"].as<JsonArrayConst>().size() != 0) {
    incoming["quota"].set(previous["quota"]);
    incoming["quota_cached"] = true;
    return true;
  }
  return false;
}

struct FingerprintWriter {
  uint32_t value = 2166136261u;
  size_t write(uint8_t byte) { value = (value ^ byte) * 16777619u; return 1; }
  size_t write(const uint8_t *bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) write(bytes[i]);
    return count;
  }
};
inline uint32_t fingerprint(const JsonDocument &snapshot) {
  FingerprintWriter writer;
  for (JsonPairConst field : snapshot.as<JsonObjectConst>()) {
    if (strcmp(field.key().c_str(), "generated_at") == 0) continue;
    writer.write(reinterpret_cast<const uint8_t *>(field.key().c_str()), strlen(field.key().c_str()) + 1);
    serializeJson(field.value(), writer);
  }
  return writer.value;
}
} // namespace dashboard_cache
