#include <cassert>
#include <cstdlib>
#include <iostream>
#include <map>
#include <vector>
#include "Memory.h"
#include "InternalAllocation.h"
#include "flow32/graphics/Display.h"

struct Allocation { size_t bytes; uint32_t caps; };
std::map<void *, Allocation> allocations;
bool psram = true;
int failAfter = -1;
size_t allocationCalls = 0;
TaskHandle_t testCurrentTask = nullptr;
unsigned taskApiCalls = 0;
unsigned ordinaryCalls = 0;
extern "C" void *__real_malloc(size_t size) { ++ordinaryCalls; return std::malloc(size); }
extern "C" void *__real_calloc(size_t count, size_t size) { ++ordinaryCalls; return std::calloc(count, size); }
extern "C" void *__wrap_malloc(size_t);
extern "C" void *__wrap_calloc(size_t, size_t);
void *heap_caps_malloc(size_t bytes, uint32_t caps) {
  ++allocationCalls;
  if ((!psram && (caps & MALLOC_CAP_SPIRAM)) || failAfter == 0) return nullptr;
  if (failAfter > 0) --failAfter;
  auto *base = static_cast<uint8_t *>(std::malloc(bytes + 32));
  if (!base) return nullptr;
  memset(base, 0xa5, bytes + 32);
  void *p = base + 16;
  if (p) allocations[p] = {bytes, caps};
  return p;
}
void *heap_caps_calloc(size_t count, size_t size, uint32_t caps) {
  void *p = heap_caps_malloc(count * size, caps);
  if (p) memset(p, 0, count * size);
  return p;
}
void heap_caps_free(void *p) {
  if (!p) return;
  const size_t bytes = allocations.at(p).bytes;
  auto *base = static_cast<uint8_t *>(p) - 16;
  for (size_t i = 0; i < 16; ++i) {
    assert(base[i] == 0xa5);
    assert(base[16 + bytes + i] == 0xa5);
  }
  assert(allocations.erase(p) == 1);
  std::free(base);
}

struct Panel : DisplayTransport {
  std::vector<uint16_t> screen = std::vector<uint16_t>(240 * 320);
  size_t pixels = 0, calls = 0;
  bool started = false;
  bool begin(const DisplayPanel &) override { started = true; return true; }
  void end() override { started = false; }
  void present(const uint16_t *data, int16_t x, int16_t y, int16_t w, int16_t h, int16_t stride) override {
    assert(started && x >= 0 && y >= 0 && x + w <= 240 && y + h <= 320);
    const auto allocation = allocations.at(const_cast<uint16_t *>(data));
    assert((allocation.caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    assert(allocation.bytes == 480 && h == 1 && stride == w);
    for (int row = 0; row < h; ++row)
      for (int col = 0; col < w; ++col) screen[(y + row) * 240 + x + col] = data[row * stride + col];
    pixels += w * h;
    ++calls;
  }
};

void testDisplay(bool external) {
  psram = external;
  Panel transport;
  DisplayPanel spec;
  spec.width = 240;
  spec.height = 320;
  {
    Display display(spec, transport);
    assert(display.begin());
    size_t internal = 0, color = 0;
    for (const auto &entry : allocations) {
      if (entry.second.caps & MALLOC_CAP_INTERNAL) internal += entry.second.bytes;
      if (entry.second.caps & MALLOC_CAP_SPIRAM) color += entry.second.bytes;
    }
    assert(internal == 10080);
    assert(color == (external ? 153600 : 0));
    const size_t before = allocationCalls;
    display.present();
    assert(transport.pixels == 76800 && transport.calls == 320);
    display.present();
    assert(transport.pixels == 76800);
    display.drawPixel(20, 20, 0xffff);
    display.present(20, 20, 1, 1);
    assert(transport.screen[20 * 240 + 20] == 0xffff);
    const size_t partial = transport.pixels;
    display.present(); // partial presentation must not lose remaining dirty tile
    assert(transport.pixels == partial + 256);
    const size_t unchanged = transport.pixels;
    display.drawPixel(20, 20, 0xffff);
    display.present();
    assert(transport.pixels == unchanged);
    display.fillRect(-4, -4, 8, 8, 0xffff);
    display.present(-4, -4, 8, 8);
    assert(transport.screen[0] == 0xffff);
    display.drawPixel(239, 319, 0xffff);
    display.present(239, 319, 100, 100);
    assert(transport.screen.back() == 0xffff);
    display.drawPixel(-1, 0, 0xffff);
    display.fillRoundRect(20, 30, 50, 40, 10, 0x07e0);
    display.present();
    for (int i = 0; i < 100; ++i) {
      display.clear(0);
      display.fillRoundRect(i, 40, 30, 30, 8, 0xffff);
      display.present();
    }
    assert(allocationCalls == before);
  }
  assert(allocations.empty() && !transport.started);
}

void testFailures() {
  psram = true;
  for (int fail = 0; fail < 3; ++fail) {
    failAfter = fail;
    Panel transport;
    DisplayPanel spec;
    spec.width = 240;
    spec.height = 320;
    Display display(spec, transport);
    assert(!display.begin());
    assert(allocations.empty());
    assert(!transport.started);
    display.clear(0xffff);
    display.present();
  }
  failAfter = -1;
}

void testArena() {
  alignas(8) uint8_t storage[65536];
  memory::Arena arena;
  arena.bind(storage, sizeof(storage));
  assert(!arena.allocate(SIZE_MAX));
  auto *p = static_cast<char *>(arena.allocate(8));
  memcpy(p, "testing", 8);
  auto *grown = static_cast<char *>(arena.reallocate(p, 256));
  assert(grown == p && strcmp(grown, "testing") == 0);
  arena.allocate(16);
  auto *moved = static_cast<char *>(arena.reallocate(grown, 512));
  assert(moved != grown && strcmp(moved, "testing") == 0);
  assert(arena.reallocate(moved, 100000) == nullptr);
  assert(strcmp(moved, "testing") == 0);
  for (int i = 0; i < 1000; ++i) {
    arena.reset();
    JsonDocument doc(&arena);
    assert(!deserializeJson(doc, "{\"type\":\"audio.start\",\"turn\":12,\"text\":\"hello\"}"));
    assert(doc["turn"].as<int>() == 12);
    assert(arena.used() < sizeof(storage));
  }
  // Exercise the exact two-arena snapshot swap used by Dashboard.
  alignas(8) uint8_t other[65536];
  memory::Arena arenas[2];
  arenas[0].bind(storage, sizeof(storage));
  arenas[1].bind(other, sizeof(other));
  JsonDocument snapshot(&arenas[0]);
  int live = 0;
  for (int i = 0; i < 1000; ++i) {
    const int spare = live ^ 1;
    arenas[spare].reset();
    JsonDocument candidate(&arenas[spare]);
    assert(!deserializeJson(candidate, "{\"protocol\":\"dashboard\",\"value\":42}"));
    snapshot = std::move(candidate);
    live = spare;
    assert(snapshot["value"].as<int>() == 42);
  }
  alignas(8) uint8_t tiny[32];
  memory::Arena bounded;
  bounded.bind(tiny, sizeof(tiny));
  JsonDocument tooLarge(&bounded);
  assert(deserializeJson(tooLarge, "{\"hello\":\"world\"}") == DeserializationError::NoMemory);
}

int main() {
  void *early = __wrap_malloc(16);
  assert(taskApiCalls == 0); // startup malloc must not access scheduler state
  std::free(early);
  memory::beginAllocationScopes();
  testCurrentTask = reinterpret_cast<void *>(1);
  {
    memory::InternalAllocationScope scope;
    void *internal = __wrap_malloc(32);
    assert(allocations.at(internal).caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    heap_caps_free(internal);
    auto *zero = static_cast<uint8_t *>(__wrap_calloc(4, 8));
    for (size_t i = 0; i < 32; ++i) assert(zero[i] == 0);
    heap_caps_free(zero);
    failAfter = 0;
    const unsigned ordinaryBefore = ordinaryCalls;
    assert(__wrap_malloc(32) == nullptr);
    assert(__wrap_calloc(1, 32) == nullptr);
    assert(ordinaryCalls == ordinaryBefore); // no fallback into PSRAM
    failAfter = -1;
    testCurrentTask = reinterpret_cast<void *>(2);
    void *unrelated = __wrap_malloc(32);
    assert(ordinaryCalls == ordinaryBefore + 1);
    std::free(unrelated);
    testCurrentTask = reinterpret_cast<void *>(1);
  }
  assert(allocations.empty());
  std::cerr << "Testing color display\n";
  testDisplay(true);
  std::cerr << "Testing monochrome display\n";
  testDisplay(false);
  std::cerr << "Testing allocation failure\n";
  testFailures();
  std::cerr << "Testing JSON arenas\n";
  testArena();
  std::cout << "PASS: task-scoped internal allocation, display placement, dirty regions, clipping, monochrome fallback, allocation failures, no drawing allocations, bounded JSON and snapshot reuse\n";
}
