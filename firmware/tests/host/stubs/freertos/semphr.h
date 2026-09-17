#pragma once
#include <mutex>
using StaticSemaphore_t = std::mutex;
using SemaphoreHandle_t = StaticSemaphore_t *;
constexpr int portMAX_DELAY = -1;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { return storage; }
inline int xSemaphoreTake(SemaphoreHandle_t mutex, int) { mutex->lock(); return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); return 1; }
