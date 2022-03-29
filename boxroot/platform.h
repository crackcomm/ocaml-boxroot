#ifndef BOXROOT_PLATFORM_H
#define BOXROOT_PLATFORM_H

#include <stddef.h>

/* Log of the size of the pools (12 = 4KB, an OS page).
   Recommended: 14. */
#define POOL_LOG_SIZE 14

#define POOL_SIZE ((size_t)1 << POOL_LOG_SIZE)
#define POOL_ALIGNMENT POOL_SIZE

#ifdef CAML_INTERNALS

#include <assert.h>
#include <limits.h>

/* Check integrity of pool structure after each scan, and print
   additional statistics? (slow)
   This can be enabled by passing BOXROOT_DEBUG=1 as argument. */
#if defined(BOXROOT_DEBUG) && (BOXROOT_DEBUG == 1)
#define DEBUG 1
#define DEBUGassert(x) assert(x)
#else
#define DEBUG 0
#define DEBUGassert(x) ((void)0)
#endif

static_assert(POOL_SIZE / sizeof(void*) <= INT_MAX, "pool size too large");

typedef struct pool pool;

pool* alloc_uninitialised_pool();
void free_pool(struct pool *p);

#endif // CAML_INTERNALS

#endif // BOXROOT_PLATFORM_H
