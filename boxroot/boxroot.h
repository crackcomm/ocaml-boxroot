/* SPDX-License-Identifier: MIT */
#ifndef BOXROOT_H
#define BOXROOT_H

#define CAML_NAME_SPACE

#include <caml/mlvalues.h>
#include "ocaml_hooks.h"
#include "platform.h"

typedef struct boxroot_private* boxroot;

/* `boxroot_create(v)` allocates a new boxroot initialised to the
   value `v`. This value will be considered as a root by the OCaml GC
   as long as the boxroot lives or until it is modified. A return
   value of `NULL` indicates a failure of allocation of the backing
   store. The OCaml domain lock must be held before calling
   `boxroot_create`. */
inline boxroot boxroot_create(value);

/* `boxroot_get(r)` returns the contained value, subject to the usual
   discipline for non-rooted values. `boxroot_get_ref(r)` returns a
   pointer to a memory cell containing the value kept alive by `r`,
   that gets updated whenever its block is moved by the OCaml GC. The
   pointer becomes invalid after any call to `boxroot_delete(r)` or
   `boxroot_modify(&r,v)`. The argument must be non-null.

   The OCaml domain lock must be held before calling `boxroot_get*`.
*/
inline value boxroot_get(boxroot r) { return *(value *)r; }
inline value const * boxroot_get_ref(boxroot r) { return (value *)r; }

/* `boxroot_delete(r)` deallocates the boxroot `r`. The value is no
   longer considered as a root by the OCaml GC. The argument must be
   non-null. (One does not need to hold the OCaml domain lock before
   calling `boxroot_delete`.)*/
inline void boxroot_delete(boxroot);

/* `boxroot_modify(&r,v)` changes the value kept alive by the boxroot
   `r` to `v`. It is equivalent to the following:
   ```
   boxroot_delete(r);
   r = boxroot_create(v);
   ```
   In particular, the root can be reallocated. However, unlike
   `boxroot_create`, `boxroot_modify` never fails, so `r` is
   guaranteed to be non-NULL afterwards. In addition, `boxroot_modify`
   is more efficient. Indeed, the reallocation, if needed, occurs at
   most once between two minor collections.

   The OCaml domain lock must be held before calling `boxroot_modify`.
*/
void boxroot_modify(boxroot *, value);


/* The behaviour of the above functions is well-defined only after the
   allocator has been initialised with `boxroot_setup` and before it
   has released its resources with `boxroot_teardown`.

   [boxroot_setup] must be called after OCaml startup while holding
   the domain lock, and [boxroot_teardown] can only be called after
   OCaml shutdown. [boxroot_setup] returns 0 if boxroot has already
   been setup or tore down, 1 otherwise.
 */
int boxroot_setup();
void boxroot_teardown();

/* Show some statistics on the standard output. */
void boxroot_print_stats();


/* Private implementation */

typedef struct {
  void *next;
  /* if non-empty, points to last cell */
  void *end;
  /* length of the list */
  int alloc_count;
#if OCAML_MULTICORE
  atomic_int domain_id;
#endif
} boxroot_fl;

extern boxroot_fl *boxroot_current_fl[Num_domains + 1];

void boxroot_create_debug(value v);
boxroot boxroot_create_slow(value v);

/* Test the overheads of multithreading (systhreads and multicore).
   Purely for experimental purposes. Otherwise should always be 1. */
#define BOXROOT_MULTITHREAD 1

inline boxroot boxroot_create(value init)
{
#if defined(BOXROOT_DEBUG) && (BOXROOT_DEBUG == 1)
  boxroot_create_debug(init);
#endif
  /* Find current freelist. Synchronized by domain lock. */
  boxroot_fl *fl = boxroot_current_fl[Domain_id];
  if (BOXROOT_MULTITHREAD && BOXROOT_UNLIKELY(fl == NULL)) goto slow;
  void *new_root = fl->next;
  if (BOXROOT_UNLIKELY(new_root == fl)) goto slow;
  fl->next = *((void **)new_root);
  fl->alloc_count++;
  *((value *)new_root) = init;
  return (boxroot)new_root;
slow:
  return boxroot_create_slow(init);
}

/* Log of the size of the pools (12 = 4KB, an OS page).
   Recommended: 14. */
#define POOL_LOG_SIZE 14
#define POOL_SIZE ((size_t)1 << POOL_LOG_SIZE)
/* Every DEALLOC_THRESHOLD deallocations, make a pool available for
   allocation or demotion into a young pool, or reclassify it as an
   empty pool if empty. Change this with benchmarks in hand. Must be a
   power of 2. */
#define DEALLOC_THRESHOLD ((int)POOL_SIZE / 2)

void boxroot_try_demote_pool(boxroot_fl *p);

#define Get_pool_header(s)                                \
  ((void *)((uintptr_t)s & ~((uintptr_t)POOL_SIZE - 1)))

#if OCAML_MULTICORE && BOXROOT_MULTITHREAD
#define dom_id_of_fl(fl) \
  atomic_load_explicit(&(fl)->domain_id, memory_order_relaxed);
#else
#define dom_id_of_fl(fl) ((void)(fl),0)
#endif

inline int boxroot_free_slot(boxroot_fl *fl, boxroot root)
{
  void **s = (void **)root;
  void *n = (void *)fl->next;
  *s = n;
  if (BOXROOT_MULTITHREAD && BOXROOT_UNLIKELY(n == fl)) fl->end = s;
  fl->next = s;
  int alloc_count = --fl->alloc_count;
  return (alloc_count & (DEALLOC_THRESHOLD - 1)) == 0;
}

void boxroot_delete_debug(boxroot root);
void boxroot_delete_slow(boxroot root);

inline void boxroot_delete(boxroot root)
{
#if defined(BOXROOT_DEBUG) && (BOXROOT_DEBUG == 1)
  boxroot_delete_debug(root);
#endif
  boxroot_fl *fl = Get_pool_header(root);
  int dom_id = dom_id_of_fl(fl);
  if ((BOXROOT_MULTITHREAD && !boxroot_domain_lock_held(dom_id))
      || BOXROOT_UNLIKELY(boxroot_free_slot(fl, root)))
    boxroot_delete_slow(root);
}

#endif // BOXROOT_H
