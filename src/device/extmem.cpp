// External PSRAM allocation for big app data (node directory, contacts, JSON, LVGL objects).
// Wi-Fi, Bluetooth and TLS keep internal RAM: this Arduino build doesn't allow them in PSRAM.
#include <esp_heap_caps.h>

#include <cstdlib>

namespace st {
void *extMalloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}
void extFree(void *p) { heap_caps_free(p); }
}  // namespace st

// ------------------------------------------------------------------------------------------------
// All C++ allocations (std::string, vectors, maps, objects made with `new`) go to PSRAM. The app's
// thousands of small strings were filling the ~300 KB of internal RAM that Wi-Fi, Bluetooth,
// TLS/AES and the display driver need. Those are C code using malloc/heap_caps directly, so they
// keep internal RAM; only C++ `new` is redirected. Falls back to internal RAM if PSRAM is full.
#include <new>

static inline void *cppAlloc(size_t n) {
  if (n == 0) n = 1;
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  return p;
}
static inline void *cppAllocAligned(size_t n, size_t a) {
  if (n == 0) n = 1;
  void *p = heap_caps_aligned_alloc(a, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_aligned_alloc(a, n, MALLOC_CAP_8BIT);
  return p;
}

void *operator new(size_t n) {
  void *p = cppAlloc(n);
  if (!p) abort();
  return p;
}
void *operator new[](size_t n) { return operator new(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return cppAlloc(n); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return cppAlloc(n); }
void *operator new(size_t n, std::align_val_t a) {
  void *p = cppAllocAligned(n, (size_t)a);
  if (!p) abort();
  return p;
}
void *operator new[](size_t n, std::align_val_t a) { return operator new(n, a); }
void *operator new(size_t n, std::align_val_t a, const std::nothrow_t &) noexcept { return cppAllocAligned(n, (size_t)a); }
void *operator new[](size_t n, std::align_val_t a, const std::nothrow_t &) noexcept { return cppAllocAligned(n, (size_t)a); }

void operator delete(void *p) noexcept { heap_caps_free(p); }
void operator delete[](void *p) noexcept { heap_caps_free(p); }
void operator delete(void *p, size_t) noexcept { heap_caps_free(p); }
void operator delete[](void *p, size_t) noexcept { heap_caps_free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { heap_caps_free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { heap_caps_free(p); }
void operator delete(void *p, std::align_val_t) noexcept { heap_caps_free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { heap_caps_free(p); }
void operator delete(void *p, size_t, std::align_val_t) noexcept { heap_caps_free(p); }
void operator delete[](void *p, size_t, std::align_val_t) noexcept { heap_caps_free(p); }
