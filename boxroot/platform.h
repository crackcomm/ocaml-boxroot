#ifndef BOXROOT_PLATFORM_H
#define BOXROOT_PLATFORM_H

#ifdef CAML_INTERNALS

#include <assert.h>
#include <limits.h>
#include <stddef.h>

/* Check integrity of pool structure after each scan, and print
   additional statistics? (slow)
   This can be enabled by passing BOXROOT_DEBUG=1 as argument. */
#if defined(BOXROOT_DEBUG) && (BOXROOT_DEBUG == 1)
#define DEBUG 1
#define DEBUGassert(x) assert(x)
#else
#define DEBUG 0
#if defined(__GNUC__)
#define DEBUGassert(x) do { if (!(x)) { __builtin_unreachable(); } } while (0)
#else
#define DEBUGassert(x) ((void)0)
#endif
#endif

#if defined(__GNUC__)
#define LIKELY(a) __builtin_expect(!!(a),1)
#define UNLIKELY(a) __builtin_expect(!!(a),0)
#else
#define LIKELY(a) (a)
#define UNLIKELY(a) (a)
#endif

typedef struct pool pool;

pool* alloc_uninitialised_pool(size_t size);
void free_pool(struct pool *p);

#endif // CAML_INTERNALS

#endif // BOXROOT_PLATFORM_H
