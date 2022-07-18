#ifndef BOXROOT_PLATFORM_H
#define BOXROOT_PLATFORM_H

#include <caml/version.h>

#if defined(__GNUC__)
#define LIKELY(a) __builtin_expect(!!(a),1)
#define UNLIKELY(a) __builtin_expect(!!(a),0)
#else
#define LIKELY(a) (a)
#define UNLIKELY(a) (a)
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
#define BOXROOT_USE_MUTEX 1

#else

#define Num_domains 1
#define Domain_id 0

/* Make it thread-safe using mutexes? Always true for OCaml multicore.
   This is needed with OCaml 4 if you want to delete a boxroot without
   holding the domain lock. This can be forced by passing
   ENABLE_BOXROOT_MUTEX=1 as argument. */
#if defined(ENABLE_BOXROOT_MUTEX) && (ENABLE_BOXROOT_MUTEX == 1)
#define BOXROOT_USE_MUTEX 1
#else
#define BOXROOT_USE_MUTEX 0
#endif // ENABLE_BOXROOT_MUTEX

#endif // OCAML_MULTICORE

#ifdef CAML_INTERNALS

#include <assert.h>
#include <stddef.h>

#if OCAML_MULTICORE
#include <stdatomic.h>
#endif

#include <caml/mlvalues.h>
#include <caml/minor_gc.h>
#include <caml/roots.h>

#if BOXROOT_USE_MUTEX

#include <pthread.h>
typedef pthread_mutex_t mutex_t;
#define BOXROOT_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER;

#else

typedef int mutex_t;
#define BOXROOT_MUTEX_INITIALIZER 0;

#endif // BOXROOT_USE_MUTEX

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
