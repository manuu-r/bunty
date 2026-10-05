#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t)=0;
  virtual size_t write(const uint8_t *data,size_t length) {
    size_t n=0; for(;n<length && write(data[n]);++n) {} return n;
  }
  size_t print(const char *s) { return write(reinterpret_cast<const uint8_t *>(s),strlen(s)); }
};
