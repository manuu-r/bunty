#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <cassert>
using TaskHandle_t = void *;
extern TaskHandle_t testCurrentTask;
extern unsigned taskApiCalls;
inline TaskHandle_t xTaskGetCurrentTaskHandle() { ++taskApiCalls; return testCurrentTask; }
#define configASSERT(value) assert(value)
using std::min;
using std::max;
struct TestSerial {
  void println(const char *) {}
  template <typename... T> void printf(const char *, T...) {}
  size_t write(const uint8_t *, size_t bytes) { return bytes; }
};
inline TestSerial Serial;
