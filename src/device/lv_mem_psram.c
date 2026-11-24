/* LVGL memory hooks: allocate from external PSRAM (falls back to internal if PSRAM is full). */
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  (void)mem;
  (void)bytes;
  return NULL;
}
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }
void *lv_malloc_core(size_t size) {
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(size);
}
void *lv_realloc_core(void *p, size_t new_size) {
  void *n = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return n ? n : realloc(p, new_size);
}
void lv_free_core(void *p) { heap_caps_free(p); }
void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { memset(mon_p, 0, sizeof(*mon_p)); }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
#endif
