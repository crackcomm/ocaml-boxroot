/* SPDX-License-Identifier: MIT */
#ifndef BOXROOT_PLATFORM_H
#define BOXROOT_PLATFORM_H

#include <caml/version.h>

#if defined(__GNUC__)
#define BOXROOT_LIKELY(a) __builtin_expect(!!(a),1)
#define BOXROOT_UNLIKELY(a) __builtin_expect(!!(a),0)
#else
#define BOXROOT_LIKELY(a) (a)
#define BOXROOT_UNLIKELY(a) (a)
#endif

#if OCAML_VERSION >= 50000
#define OCAML_MULTICORE 1
#else
#define OCAML_MULTICORE 0
#endif

#if OCAML_MULTICORE

/* We currently rely on OCaml 5.0 having a max number of domains; this
   is checked for consistency. */
#define Num_domains 128
#define Domain_id (Caml_state->id)

#else

#define Num_domains 1
#define Domain_id 0

#endif // OCAML_MULTICORE

#ifdef CAML_INTERNALS

#include <assert.h>
#include <stddef.h>
#include <caml/mlvalues.h>
#include <caml/minor_gc.h>
#include <caml/roots.h>

#if OCAML_MULTICORE

#include <stdatomic.h>

typedef atomic_llong stat_t;

static inline long long incr(stat_t *n)
{
  return 1 + atomic_fetch_add_explicit(n, 1, memory_order_relaxed);
}

static inline long long decr(stat_t *n)
{
  return atomic_fetch_add_explicit(n, -1, memory_order_relaxed) - 1;
}

#else

typedef long long stat_t;

static inline long long incr(stat_t *n) { return ++(*n); }
static inline long long decr(stat_t *n) { return --(*n); }

#endif // OCAML_MULTICORE

#include <pthread.h>
typedef pthread_mutex_t mutex_t;
#define BOXROOT_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER;

int boxroot_initialize_mutex(mutex_t *mutex);
void boxroot_mutex_lock(mutex_t *mutex);
void boxroot_mutex_unlock(mutex_t *mutex);

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

typedef struct pool pool;

pool* boxroot_alloc_uninitialised_pool(size_t size);
void boxroot_free_pool(pool *p);

#endif // CAML_INTERNALS

#endif // BOXROOT_PLATFORM_H
