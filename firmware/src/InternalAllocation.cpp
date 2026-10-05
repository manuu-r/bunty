#include "InternalAllocation.h"
#include "Runtime.h"
#include <atomic>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>

namespace {
StaticSemaphore_t scopeMutexStorage;
SemaphoreHandle_t scopeMutex = nullptr;
std::atomic<TaskHandle_t> internalOwner{nullptr};

bool internalOnly() {
  const TaskHandle_t owner = internalOwner.load(std::memory_order_acquire);
  // malloc is also called before the scheduler starts. Do not touch task
  // APIs unless a scope has explicitly been entered after setup began.
  return owner && owner == xTaskGetCurrentTaskHandle();
}
}

void memory::beginAllocationScopes() {
  scopeMutex = xSemaphoreCreateMutexStatic(&scopeMutexStorage);
  configASSERT(scopeMutex);
}
memory::InternalAllocationScope::InternalAllocationScope() {
  configASSERT(scopeMutex);
  xSemaphoreTake(scopeMutex, portMAX_DELAY);
  internalOwner.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
}
memory::InternalAllocationScope::~InternalAllocationScope() {
  internalOwner.store(nullptr, std::memory_order_release);
  xSemaphoreGive(scopeMutex);
}

extern "C" {
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__wrap_malloc(size_t size) {
  return internalOnly() ? heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                        : __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
  return internalOnly() ? heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                        : __real_calloc(count, size);
}
}
