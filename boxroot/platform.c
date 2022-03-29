#define CAML_INTERNALS

#include "platform.h"
#include <stdlib.h>
#include <errno.h>

pool * alloc_uninitialised_pool()
{
  void *p = NULL;
  // TODO: portability?
  // Win32: p = _aligned_malloc(size, alignment);
  int err = posix_memalign(&p, POOL_ALIGNMENT, POOL_SIZE);
  assert(err != EINVAL);
  if (err == ENOMEM) return NULL;
  assert(p != NULL);
  return p;
}

void free_pool(pool *p) {
    // Win32: _aligned_free(p);
    free(p);
}
