#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <new>

namespace memory {
constexpr uint32_t kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kExternal = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
// Bulk data must never silently consume the control/DMA heap.
inline void *external(size_t bytes) { return heap_caps_malloc(bytes, kExternal); }
inline void *internal(size_t bytes) { return heap_caps_malloc(bytes, kInternal); }
void sampleTask(const char *name);
void report();

// A bounded, reusable ArduinoJson arena. No system heap calls during parsing.
// The document must be destroyed BEFORE reset; realloc preserves old contents.
class Arena : public ArduinoJson::Allocator {
 public:
  void bind(void *data, size_t capacity) { data_ = static_cast<uint8_t *>(data); capacity_ = capacity; reset(); }
  void reset() { used_ = 0; }
  size_t used() const { return used_; }
  void *allocate(size_t size) override {
    if (size > SIZE_MAX - 7) return nullptr;
    const size_t padded = (size + 7) & ~size_t(7);
    if (!data_ || padded > capacity_ || used_ > capacity_ - padded ||
        capacity_ - used_ - padded < 8) return nullptr;
    auto *header = reinterpret_cast<size_t *>(data_ + used_);
    *header = size;
    used_ += padded + 8;
    return reinterpret_cast<uint8_t *>(header) + 8;
  }
  void deallocate(void *) override {}
  void *reallocate(void *old, size_t size) override {
    if (!old) return allocate(size);
    const size_t previous = *reinterpret_cast<size_t *>(static_cast<uint8_t *>(old) - 8);
    if (size <= previous) return old;
    if (size > SIZE_MAX - 7) return nullptr;
    const size_t offset = static_cast<uint8_t *>(old) - data_;
    const size_t padded = (size + 7) & ~size_t(7);
    if (offset + ((previous + 7) & ~size_t(7)) == used_ &&
        offset <= capacity_ && padded <= capacity_ - offset) {
      used_ = offset + padded;
      *reinterpret_cast<size_t *>(static_cast<uint8_t *>(old) - 8) = size;
      return old;
    }
    void *next = allocate(size);
    if (next) memcpy(next, old, previous);
    return next;
  }
 private:
  uint8_t *data_ = nullptr;
  size_t capacity_ = 0;
  size_t used_ = 0;
};
}  // namespace memory
