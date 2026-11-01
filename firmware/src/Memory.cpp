#include "Log.h"
#include "Memory.h"
#include "Ui.h"
#include "flow32/graphics/Display.h"

namespace memory {
namespace {
struct TaskSample { const char *name = nullptr; uint32_t minimum = UINT32_MAX; int core = -1; uint32_t sampled = 0; };
TaskSample tasks[12];
std::atomic<uint32_t> loopPeakUs{0}, loopSlowCount{0};
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
}
void recordLoop(uint32_t microseconds) {
  // One writer: main task. Reports may exchange counters from another task.
  uint32_t peak=loopPeakUs.load();
  while(microseconds>peak && !loopPeakUs.compare_exchange_weak(peak,microseconds)) {}
  if(microseconds>50000) ++loopSlowCount;
}
void sampleTask(const char *name) {
  const uint32_t now = millis();
  int slot = -1;
  portENTER_CRITICAL(&mux);
  for (int i = 0; i < 12; ++i) {
    if (!tasks[i].name || strcmp(tasks[i].name, name) == 0) {
      tasks[i].name = name;
      if (tasks[i].sampled == 0 || now - tasks[i].sampled >= 5000) {
        tasks[i].sampled = now;
        slot = i;
      }
      break;
    }
  }
  portEXIT_CRITICAL(&mux);
  if (slot < 0) return;
  // ESP-IDF's high-water mark is in bytes, unlike upstream FreeRTOS.
  const uint32_t remaining = uxTaskGetStackHighWaterMark(nullptr);
  portENTER_CRITICAL(&mux);
  tasks[slot].minimum = min(tasks[slot].minimum, remaining);
  tasks[slot].core = xPortGetCoreID();
  portEXIT_CRITICAL(&mux);
}
void report() {
  logPrintf("[runtime] loop_peak_us=%u loops_over_50ms=%u\n",
            unsigned(loopPeakUs.exchange(0)),unsigned(loopSlowCount.exchange(0)));
  for (uint32_t caps : {kInternal, kExternal, uint32_t(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)}) {
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, caps);
    const unsigned fragmentation = info.total_free_bytes == 0 ? 0 :
        100 - (100ULL * info.largest_free_block / info.total_free_bytes);
    logPrintf("[memory] %s free=%u min=%u largest=%u frag=%u%% allocated=%u\n",
        caps == kInternal ? "internal" : caps == kExternal ? "psram" : "dma",
        unsigned(info.total_free_bytes), unsigned(info.minimum_free_bytes),
        unsigned(info.largest_free_block), fragmentation, unsigned(info.total_allocated_bytes));
  }
  TaskSample copy[12];
  portENTER_CRITICAL(&mux);
  memcpy(copy, tasks, sizeof(copy));
  portEXIT_CRITICAL(&mux);
  for (const auto &task : copy) if (task.name)
    logPrintf("[memory] task=%s core=%d stack_min_free=%u bytes\n", task.name, task.core, unsigned(task.minimum));
  ui::Guard guard;
  ui::display().reportMemory();
}
}
