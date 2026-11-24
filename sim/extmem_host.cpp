// Desktop simulator: "external RAM" is just the normal heap.
#include <cstdlib>
namespace st {
void *extMalloc(size_t n) { return malloc(n); }
void extFree(void *p) { free(p); }
}  // namespace st
