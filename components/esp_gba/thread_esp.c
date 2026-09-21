/* Threading shim for the core's THREADED_RENDERER, pinned to the second core.
 *
 * The ESP32-S3 is dual core and the emulator has only ever used core 0 (PRO);
 * core 1 (APP) has been idle this whole time. vba-next can run its scanline
 * renderer on a separate thread, so pinning that thread to core 1 does the
 * PPU work in parallel with CPU emulation instead of serially after it.
 *
 * Upstream's thread.c targets Vita or libretro's rthreads; neither is built
 * here, so these four symbols are all that is needed.
 *
 * MEASURED AND CURRENTLY DISABLED. With the renderer on core 1, Emerald went
 * from 11.1-11.6 emulated fps to 12.3-14.6 (+11%), but DRAWN frames fell from
 * 11 to 6-7 -- the renderer could not keep pace with the CPU thread, so the
 * picture updated half as often. Faster on paper, visibly worse. Re-enable with
 * -DTHREADED_RENDERER=1 in both CMakeLists if the sync is ever improved.
 */
#include "thread.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef THREADED_RENDERER

#define RENDER_CORE 1
#define RENDER_STACK 4096

struct shim_args {
  threadfunc_t func;
  void *arg;
};

static void shim_entry(void *p) {
  struct shim_args *a = (struct shim_args *)p;
  threadfunc_t f = a->func;
  void *arg = a->arg;
  vPortFree(a);
  ESP_LOGI("THREAD", "renderer running on core %d", xPortGetCoreID());
  f(arg);
  vTaskDelete(NULL);
}

/* FreeRTOS priorities run the other way round from the core's scale (1 =
 * highest there), so invert. Stay below the IDF timer/ipc tasks. */
static UBaseType_t map_priority(int priority) {
  int p = 6 - priority; /* HIGHEST(1) -> 5, LOWEST(5) -> 1 */
  if (p < 1) p = 1;
  if (p > 5) p = 5;
  return (UBaseType_t)p;
}

thread_t thread_run(threadfunc_t func, void *p, int priority) {
  struct shim_args *a = (struct shim_args *)pvPortMalloc(sizeof(*a));
  if (!a) {
    return NULL;
  }
  a->func = func;
  a->arg = p;

  TaskHandle_t h = NULL;
  if (xTaskCreatePinnedToCore(shim_entry, "gba_render", RENDER_STACK, a,
                              map_priority(priority), &h,
                              RENDER_CORE) != pdPASS) {
    vPortFree(a);
    return NULL;
  }
  return (thread_t)h;
}

thread_t thread_get(void) { return (thread_t)xTaskGetCurrentTaskHandle(); }

void thread_sleep(int ms) {
  /* The renderer spins waiting for work; yielding for a tick would cost far
   * more than the wait itself, so give up the CPU for the shortest time the
   * scheduler allows. */
  vTaskDelay(ms > 0 ? pdMS_TO_TICKS(ms) : 1);
}

void thread_set_priority(thread_t id, int priority) {
  if (id) {
    vTaskPrioritySet((TaskHandle_t)id, map_priority(priority));
  }
}

#endif /* THREADED_RENDERER */
