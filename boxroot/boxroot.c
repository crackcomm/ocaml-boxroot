/* SPDX-License-Identifier: MIT */
/* {{{ Includes */

// This is emacs folding-mode

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CAML_NAME_SPACE
#define CAML_INTERNALS

#include "boxroot.h"
#include <caml/minor_gc.h>
#include <caml/major_gc.h>

#if defined(_POSIX_TIMERS) && defined(_POSIX_MONOTONIC_CLOCK)
#define POSIX_CLOCK
#include <time.h>
#endif

#include "ocaml_hooks.h"
#include "platform.h"

/* }}} */

/* {{{ Data types */

typedef enum class {
  YOUNG,
  OLD,
  UNTRACKED
} class;

typedef void * slot;

struct header {
  boxroot_fl free_list;
  class class;
  struct pool *prev;
  struct pool *next;
};

static_assert(POOL_SIZE / sizeof(slot) <= INT_MAX, "pool size too large");

#define POOL_ROOTS_CAPACITY                                 \
  ((int)((POOL_SIZE - sizeof(struct header)) / sizeof(slot)))

typedef struct pool {
  struct header hd;
  /* Occupied slots are OCaml values.
     Unoccupied slots are a pointer to the next slot in the free list,
     or to pool itself, denoting the empty free list. */
  slot roots[POOL_ROOTS_CAPACITY];
} pool;

static_assert(sizeof(pool) == POOL_SIZE, "bad pool size");

/* }}} */

/* {{{ Globals */

/* Global pool rings. */
typedef struct {
  mutex_t mutex;
  /* Pool of old values: contains only roots pointing to the major
     heap. Scanned at the start of major collection. */
  pool *old;
  /* Pool of young values: contains roots pointing to the major or to
     the minor heap. Scanned at the start of minor and major
     collection. */
  pool *young;
  /* Current pool. Ring of size 1. */
  pool *current;
  /* Pools containing no root: not scanned.
     We could free these pools immediately, but this could lead to
     stuttering behavior for workloads that regularly come back to
     0 boxroots alive. Instead we wait for the next major slice to free
     empty pools.
  */
  pool *free;
} pool_rings;

static pool_rings *pools[Num_domains + 1] = { NULL };
#define Orphaned_id Num_domains

static boxroot_fl empty_fl =
  { (slot)&empty_fl
    , -1
#if OCAML_MULTICORE
    , -1
#endif
  };

/* Domain 0 can be allocated-to without a mutex with OCaml 4; make
   sure to have a clean error if boxroot is not setup. */
boxroot_fl *boxroot_current_fl[Num_domains + 1] = { &empty_fl /*, NULL... */ };

#if OCAML_MULTICORE

static inline int dom_id_of_pool(pool *p)
{
  return atomic_load_explicit(&p->hd.free_list.domain_id,
                              memory_order_relaxed);
}

static inline void pool_set_dom_id(pool *p, int dom_id)
{
  return atomic_store_explicit(&p->hd.free_list.domain_id, dom_id,
                               memory_order_relaxed);
}

#else

static inline int dom_id_of_pool(pool *p) { (void)p; return 0; }
static inline void pool_set_dom_id(pool *p, int n) { (void)p; (void)n; }

#endif // OCAML_MULTICORE

static pool_rings * init_pool_rings(int dom_id);

static inline void acquire_pool_rings(int dom_id)
{
  boxroot_mutex_lock(&pools[dom_id]->mutex);
}

static inline void release_pool_rings(int dom_id)
{
  boxroot_mutex_unlock(&pools[dom_id]->mutex);
}

static inline int acquire_pool_rings_of_pool(pool *p)
{
  int dom_id = dom_id_of_pool(p);
  while (1) {
    DEBUGassert(pools[dom_id] != NULL);
    acquire_pool_rings(dom_id);
    int new_dom_id = dom_id_of_pool(p);
    if (dom_id == new_dom_id) return dom_id;
    /* Pool owner has changed before we could lock it. Try again. */
    release_pool_rings(dom_id);
    dom_id = new_dom_id;
  }
}

static pool_rings * alloc_pool_rings()
{
  pool_rings *ps = (pool_rings *)malloc(sizeof(pool_rings));
  if (ps == NULL) goto out_err;
  if (!boxroot_initialize_mutex(&ps->mutex)) goto out_err;
  return ps;
 out_err:
  free(ps);
  return NULL;
}

static pool_rings * init_pool_rings(int dom_id)
{
  if (dom_id != Orphaned_id) assert_domain_lock_held(dom_id);
  pool_rings *local = pools[dom_id];
  if (local == NULL) local = alloc_pool_rings();
  if (local == NULL) return NULL;
  local->old = NULL;
  local->young = NULL;
  local->current = NULL;
  local->free = NULL;
  boxroot_current_fl[dom_id] = &empty_fl;
  pools[dom_id] = local;
  return local;
}

static struct {
  stat_t minor_collections;
  stat_t major_collections;
  stat_t total_create_young;
  stat_t total_create_old;
  stat_t total_delete_young;
  stat_t total_delete_old;
  stat_t total_modify;
  stat_t total_scanning_work_minor;
  stat_t total_scanning_work_major;
  stat_t total_minor_time;
  stat_t total_major_time;
  stat_t peak_minor_time;
  stat_t peak_major_time;
  stat_t total_alloced_pools;
  stat_t total_emptied_pools;
  stat_t total_freed_pools;
  stat_t live_pools; // number of tracked pools
  stat_t peak_pools; // max live pools at any time
  stat_t ring_operations; // Number of times hd.next is mutated
  stat_t young_hit_gen; /* number of times a young value was encountered
                           during generic scanning (not minor collection) */
  stat_t young_hit_young; /* number of times a young value was encountered
                             during young scanning (minor collection) */
  stat_t get_pool_header; // number of times get_pool_header was called
  stat_t is_pool_member; // number of times is_pool_member was called
  stat_t is_empty_free_list; // number of times is_empty_free_list was called
} stats;

/* }}} */

/* {{{ Tests in the hot path */

// hot path
static inline pool * get_pool_header(slot *s)
{
  if (DEBUG) incr(&stats.get_pool_header);
  return Get_pool_header(s);
}

// Return true iff v shares the same msbs as p and is not an
// immediate.
// hot path
static inline int is_pool_member(slot v, pool *p)
{
  if (DEBUG) incr(&stats.is_pool_member);
  return (uintptr_t)p == ((uintptr_t)v & ~((uintptr_t)POOL_SIZE - 2));
}

// hot path
static inline int is_empty_free_list(slot *v, pool *p)
{
  if (DEBUG) incr(&stats.is_empty_free_list);
  return (v == (slot *)p);
}

/* }}} */

/* {{{ Ring operations */

static inline void ring_link(pool *p, pool *q)
{
  p->hd.next = q;
  q->hd.prev = p;
  incr(&stats.ring_operations);
}

/* insert the ring [source] at the back of [*target]. */
static inline void ring_push_back(pool *source, pool **target)
{
  if (source == NULL) return;
  DEBUGassert(source->hd.prev == source && source->hd.next == source);
  DEBUGassert(source != *target);
  if (*target == NULL) {
    *target = source;
  } else {
    DEBUGassert((*target)->hd.class == source->hd.class);
    pool *target_last = (*target)->hd.prev;
    pool *source_last = source->hd.prev;
    ring_link(target_last, source);
    ring_link(source_last, *target);
  }
}

/* insert the ring [source] at the front of [*target]. */
static inline void ring_push_front(pool *source, pool **target)
{
  if (source == NULL) return;
  ring_push_back(source, target);
  *target = source;
}

// remove the first element from [*target] and return it
static pool * ring_pop(pool **target)
{
  pool *front = *target;
  DEBUGassert(front != NULL);
  if (front->hd.next == front) {
    *target = NULL;
  } else {
    *target = front->hd.next;
    ring_link(front->hd.prev, front->hd.next);
  }
  ring_link(front, front);
  return front;
}

/* }}} */

/* {{{ Pool management */

static pool * get_uninitialised_pool()
{
  pool *p = boxroot_alloc_uninitialised_pool(POOL_SIZE);
  if (p == NULL) return NULL;
  incr(&stats.total_alloced_pools);
  ring_link(p, p);
  p->hd.free_list.next = NULL;
  p->hd.free_list.alloc_count = 0;
  pool_set_dom_id(p, -1);
  p->hd.class = UNTRACKED;
  return p;
}

/* the empty free-list for a pool p is denoted by a pointer to the
   pool itself (NULL could be a valid value for an element slot) */
static inline slot empty_free_list(pool *p) { return (slot)p; }

static inline int is_full_pool(pool *p)
{
  return is_empty_free_list(p->hd.free_list.next, p);
}

static pool * get_empty_pool()
{
  long long live_pools = incr(&stats.live_pools);
  /* racy, but whatever */
  if (live_pools > stats.peak_pools) stats.peak_pools = live_pools;
  pool *out = get_uninitialised_pool();

  if (out == NULL) return NULL;

  out->roots[POOL_ROOTS_CAPACITY - 1] = empty_free_list(out);
  for (slot *s = out->roots + POOL_ROOTS_CAPACITY - 2; s >= out->roots; --s) {
    *s = (slot)(s + 1);
  }
  out->hd.free_list.next = out->roots;
  return out;
}

static void free_pool_ring(pool **ring)
{
  while (*ring != NULL) {
      pool *p = ring_pop(ring);
      boxroot_free_pool(p);
      incr(&stats.total_freed_pools);
  }
}

static void free_pool_rings(pool_rings *ps)
{
  free_pool_ring(&ps->old);
  free_pool_ring(&ps->young);
  free_pool_ring(&ps->current);
  free_pool_ring(&ps->free);
}

/* }}} */

/* {{{ Pool class management */

static inline int is_not_too_full(pool *p)
{
  return p->hd.free_list.alloc_count <= (int)(DEALLOC_THRESHOLD / sizeof(slot));
}

static void set_current_pool(int dom_id, pool *p)
{
  DEBUGassert(pools[dom_id]->current == NULL);
  if (p != NULL) {
    pool_set_dom_id(p, dom_id);
    pools[dom_id]->current = p;
    p->hd.class = YOUNG;
    boxroot_current_fl[dom_id] = &p->hd.free_list;
  } else {
    boxroot_current_fl[dom_id] = &empty_fl;
  }
}

/* Move not-too-full pools to the front; move empty pools to the free
   ring. */
static void try_demote_pool(pool *p)
{
  DEBUGassert(p->hd.class != UNTRACKED);
  int dom_id = dom_id_of_pool(p);
  pool_rings *remote = pools[dom_id];
  if (p == remote->current || !is_not_too_full(p)) return;
  pool **source = (p->hd.class == OLD) ? &remote->old : &remote->young;
  pool **target;
  if (p->hd.free_list.alloc_count == 0) {
    /* Move to the empty list */
    target = &remote->free;
    p->hd.class = UNTRACKED;
    incr(&stats.total_emptied_pools);
  } else {
    /* Make available by moving to the front */
    target = source;
  }
  pool *tail = p;
  ring_pop(&tail);
  if (p == *source) *source = tail;
  ring_push_front(p, target);
}

void boxroot_try_demote_pool(boxroot_fl *fl)
{
  try_demote_pool((pool *)fl);
}

static inline pool * pop_available(pool **target)
{
  /* When pools empty themselves enough, they are pushed to the front.
     When they fill up, they are pushed to the back. Thus, if the
     first one is full, then none of the next ones are empty
     enough. */
  if (*target == NULL || is_full_pool(*target)) return NULL;
  return ring_pop(target);
}

/* Find an available pool and set it as current. Return NULL if none
   was found and the allocation of a new one failed. */
static pool * find_available_pool(int dom_id)
{
  pool_rings *local = pools[dom_id];
  pool *p = pop_available(&local->young);
  if (p == NULL && local->old != NULL && is_not_too_full(local->old))
    p = pop_available(&local->old);
  if (p == NULL) p = pop_available(&local->free);
  if (p == NULL) p = get_empty_pool();
  DEBUGassert(local->current == NULL);
  set_current_pool(dom_id, p);
  return p;
}

static void validate_all_pools(int dom_id);

static void reclassify_pool(pool **source, int dom_id, class cl)
{
  DEBUGassert(*source != NULL);
  pool_rings *local = pools[dom_id];
  pool *p = ring_pop(source);
  pool_set_dom_id(p, dom_id);
  pool **target = (cl == YOUNG) ? &local->young : &local->old;
  p->hd.class = cl;
  if (is_not_too_full(p)) {
    ring_push_front(p, target);
  } else {
    ring_push_back(p, target);
  }
}

static void promote_young_pools(int dom_id)
{
  pool_rings *local = pools[dom_id];
  // Promote full pools
  while (local->young != NULL) {
    reclassify_pool(&local->young, dom_id, OLD);
  }
  // Heuristic: if a young pool has just been allocated, it is better
  // if it is the first one to be considered next time a young boxroot
  // allocation takes place. So we promote the current pool last.
  if (local->current != NULL) {
    reclassify_pool(&local->current, dom_id, OLD);
    set_current_pool(dom_id, NULL);
  }
  // A domain that does not use any boxroot between two minor
  // collections should not have to pay the cost of scanning any pool.
  DEBUGassert(local->young == NULL && local->current == NULL);
}

/* }}} */

/* {{{ Allocation, deallocation */

enum status { NOT_SETUP, RUNNING, FREED };

/* Thread-safety: see documented constraints on the use of
   boxroot_setup and boxroot_teardown. */
#if OCAML_MULTICORE
static atomic_int status = NOT_SETUP;
#else
static int status = NOT_SETUP;
#endif

// Set an available pool as current and allocate from it.
boxroot boxroot_alloc_slot_slow(value init)
{
  // We might be here because boxroot is not setup.
  if (status != RUNNING) return NULL;
#if !OCAML_MULTICORE
  boxroot_check_thread_hooks();
#endif
  int dom_id = Domain_id;
  pool_rings *local = pools[dom_id];
  if (local->current != NULL) {
    DEBUGassert(is_full_pool(local->current));
    reclassify_pool(&local->current, dom_id, YOUNG);
  }
  pool *p = find_available_pool(dom_id);
  if (p == NULL) return NULL;
  DEBUGassert(!is_full_pool(p));
  return boxroot_alloc_slot(&p->hd.free_list, init);
}

/* }}} */

/* {{{ Boxroot API implementation */

extern inline value boxroot_get(boxroot root);
extern inline value const * boxroot_get_ref(boxroot root);

extern inline boxroot boxroot_alloc_slot(boxroot_fl *fl, value init);

boxroot boxroot_create_noinline(value init)
{
  int dom_id = Domain_id;
  if (DEBUG) {
    assert_domain_lock_held(dom_id);
    if (Is_block(init) && Is_young(init)) incr(&stats.total_create_young);
    else incr(&stats.total_create_old);
  }
  /* Find current freelist. Synchronized by domain lock. */
  boxroot_fl *fl = boxroot_current_fl[dom_id];
  if (UNLIKELY(fl == NULL)) {
    pool_rings *local = init_pool_rings(dom_id);
    if (local == NULL) return NULL;
    fl = &local->current->hd.free_list;
  }
  acquire_pool_rings(dom_id);
  boxroot br = boxroot_alloc_slot(fl, init);
  release_pool_rings(dom_id);
  return br;
}

extern inline boxroot boxroot_create(value init);

extern inline void boxroot_free_slot(boxroot_fl *fl, slot *s);

void boxroot_delete_noinline(boxroot root)
{
  pool *p = get_pool_header((slot *)root);
  int dom_id = acquire_pool_rings_of_pool(p);
  DEBUGassert(root != NULL);
  if (DEBUG) {
    value v = boxroot_get(root);
    if (Is_block(v) && Is_young(v)) incr(&stats.total_delete_young);
    else incr(&stats.total_delete_old);
  }
  boxroot_free_slot(&p->hd.free_list, (slot *)root);
  release_pool_rings(dom_id);
}

extern inline void boxroot_delete(boxroot root);

static void boxroot_reallocate(boxroot *root, value new_value, int dom_id)
{
  /* We allocate in the remote pool_rings since we already hold their
     lock. */
  boxroot_fl *fl = boxroot_current_fl[dom_id];
  boxroot new = boxroot_alloc_slot(fl, new_value);
  pool *p = get_pool_header((slot *)*root);
  DEBUGassert(dom_id_of_pool(p) == dom_id);
  if (LIKELY(new != NULL)) {
    boxroot_free_slot(&p->hd.free_list, (slot *)*root);
    *root = new;
  } else {
    // Better not fail in boxroot_modify. Expensive but fail-safe:
    // demote its pool into the young pools.
    DEBUGassert(p->hd.class == OLD);
    pool_rings *remote = pools[dom_id];
    pool **source = (p == remote->old) ? &remote->old : &p;
    reclassify_pool(source, dom_id, YOUNG);
    **((value **)root) = new_value;
  }
}

// hot path
void boxroot_modify(boxroot *root, value new_value)
{
  slot *s = (slot *)*root;
  pool *p = get_pool_header(s);
  int dom_id = acquire_pool_rings_of_pool(p);
  DEBUGassert(s);
  if (DEBUG) incr(&stats.total_modify);
  if (LIKELY(p->hd.class == YOUNG
             || !Is_block(new_value)
             || !Is_young(new_value))) {
    *(value *)s = new_value;
  } else {
    // We need to reallocate, but this reallocation happens at most once
    // between two minor collections.
    boxroot_reallocate(root, new_value, dom_id);
  }
  release_pool_rings(dom_id);
}

/* }}} */

/* {{{ Scanning */

static void validate_pool(pool *pl)
{
  if (pl->hd.free_list.next == NULL) {
    // an unintialised pool
    assert(pl->hd.class == UNTRACKED);
    return;
  }
  // check freelist structure and length
  slot *curr = pl->hd.free_list.next;
  int pos = 0;
  for (; !is_empty_free_list(curr, pl); curr = (slot*)*curr, pos++)
  {
    assert(pos < POOL_ROOTS_CAPACITY);
    assert(curr >= pl->roots && curr < pl->roots + POOL_ROOTS_CAPACITY);
  }
  assert(pos == POOL_ROOTS_CAPACITY - pl->hd.free_list.alloc_count);
  // check count of allocated elements
  int alloc_count = 0;
  for(int i = 0; i < POOL_ROOTS_CAPACITY; i++) {
    slot s = pl->roots[i];
    --stats.is_pool_member;
    if (!is_pool_member(s, pl)) {
      value v = (value)s;
      if (pl->hd.class != YOUNG && Is_block(v)) assert(!Is_young(v));
      ++alloc_count;
    }
  }
  assert(alloc_count == pl->hd.free_list.alloc_count);
}

static void validate_ring(pool **ring, int dom_id, class cl)
{
  pool *start_pool = *ring;
  if (start_pool == NULL) return;
  pool *p = start_pool;
  do {
    assert(dom_id_of_pool(p) == dom_id);
    assert(p->hd.class == cl);
    validate_pool(p);
    assert(p->hd.next != NULL);
    assert(p->hd.next->hd.prev == p);
    assert(p->hd.prev != NULL);
    assert(p->hd.prev->hd.next == p);
    p = p->hd.next;
  } while (p != start_pool);
}

static void validate_all_pools(int dom_id)
{
  pool_rings *local = pools[dom_id];
  validate_ring(&local->old, dom_id, OLD);
  validate_ring(&local->young, dom_id, YOUNG);
  validate_ring(&local->current, dom_id, YOUNG);
  validate_ring(&local->free, dom_id, UNTRACKED);
}

static void orphan_pools(int dom_id)
{
  pool_rings *local = pools[dom_id]; /* synchronised by domain lock */
  if (local == NULL) return;
  acquire_pool_rings(dom_id);
  acquire_pool_rings(Orphaned_id);
  pool_rings *orphaned = pools[Orphaned_id];
  /* Move active pools to the orphaned pools. TODO: NUMA awareness? */
  ring_push_back(local->old, &orphaned->old);
  ring_push_back(local->young, &orphaned->young);
  ring_push_back(local->current, &orphaned->young);
  release_pool_rings(Orphaned_id);
  /* Free the rest */
  free_pool_ring(&local->free);
  /* Reset local pools for later domains spawning with the same id */
  init_pool_rings(dom_id);
  release_pool_rings(dom_id);
}

static void adopt_orphaned_pools(int dom_id)
{
  acquire_pool_rings(Orphaned_id);
  pool_rings *orphaned = pools[Orphaned_id];
  while (orphaned->old != NULL)
    reclassify_pool(&orphaned->old, dom_id, OLD);
  while (orphaned->young != NULL)
    reclassify_pool(&orphaned->young, dom_id, YOUNG);
  release_pool_rings(Orphaned_id);
}

// returns the amount of work done
static int scan_pool_gen(scanning_action action, void *data, pool *pl)
{
  int allocs_to_find = pl->hd.free_list.alloc_count;
  int young_hit = 0;
  slot *current = pl->roots;
  while (allocs_to_find) {
    // hot path
    slot s = *current;
    if (!is_pool_member(s, pl)) {
      --allocs_to_find;
      value v = (value)s;
      if (DEBUG && Is_block(v) && Is_young(v)) ++young_hit;
      CALL_GC_ACTION(action, data, v, (value *)current);
    }
    ++current;
  }
  stats.young_hit_gen += young_hit;
  return current - pl->roots;
}

/* Specialised version of [scan_pool_gen] when [only_young].

   Benchmark results for minor scanning:
   20% faster for young hits=95%
   20% faster for young hits=50% (random)
   90% faster for young_hit=10% (random)
   280% faster for young hits=0%
*/
static int scan_pool_young(scanning_action action, void *data, pool *pl)
{
#if OCAML_MULTICORE
  /* If a <= b - 2 then
     a < x && x < b  <=>  x - a - 1 <= x - b - 2 (unsigned comparison)
  */
  uintnat young_start = (uintnat)caml_minor_heaps_start + 1;
  uintnat young_range = (uintnat)caml_minor_heaps_end - 1 - young_start;
#else
  uintnat young_start = (uintnat)Caml_state->young_start;
  uintnat young_range = (uintnat)Caml_state->young_end - young_start;
#endif
  slot *start = pl->roots;
  slot *end = start + POOL_ROOTS_CAPACITY;
  int young_hit = 0;
  slot *i;
  for (i = start; i < end; i++) {
    slot s = *i;
    value v = (value)s;
    /* Optimise for branch prediction: if v falls within the young
       range, then it is likely that it is a block */
    if ((uintnat)v - young_start <= young_range && LIKELY(Is_block(v))) {
      ++young_hit;
      CALL_GC_ACTION(action, data, v, (value *)i);
    }
  }
  stats.young_hit_young += young_hit;
  return i - start;
}

static int scan_pool(scanning_action action, int only_young, void *data,
                     pool *pl)
{
  if (only_young)
    return scan_pool_young(action, data, pl);
  else
    return scan_pool_gen(action, data, pl);
}

static int scan_ring(scanning_action action, int only_young,
                     void *data, pool **ring)
{
  int work = 0;
  pool *start_pool = *ring;
  if (start_pool == NULL) return 0;
  pool *p = start_pool;
  do {
    work += scan_pool(action, only_young, data, p);
    p = p->hd.next;
  } while (p != start_pool);
  return work;
}

static int scan_pools(scanning_action action, int only_young,
                      void *data, int dom_id)
{
  pool_rings *local = pools[dom_id];
  int work = 0;
  work += scan_ring(action, only_young, data, &local->current);
  work += scan_ring(action, only_young, data, &local->young);
  if (!only_young) work += scan_ring(action, 0, data, &local->old);
  return work;
}

static void scan_roots(scanning_action action, int only_young,
                       void *data, int dom_id)
{
  if (DEBUG) validate_all_pools(dom_id);
  adopt_orphaned_pools(dom_id);
  int work = scan_pools(action, only_young, data, dom_id);
  if (boxroot_in_minor_collection()) {
    promote_young_pools(dom_id);
  } else {
    free_pool_ring(&pools[dom_id]->free);
  }
  if (only_young) stats.total_scanning_work_minor += work;
  else stats.total_scanning_work_major += work;
  if (DEBUG) validate_all_pools(dom_id);
}

/* }}} */

/* {{{ Statistics */

static long long time_counter(void)
{
#if defined(POSIX_CLOCK)
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * (long long)1000000000 + (long long)t.tv_nsec;
#else
  return 0;
#endif
}

// unit: 1=KiB, 2=MiB
static long long kib_of_pools(long long count, int unit)
{
  int log_per_pool = POOL_LOG_SIZE - unit * 10;
  if (log_per_pool >= 0) return count << log_per_pool;
  else return count >> -log_per_pool;
}

static double average(long long total, long long units)
{
  // round to nearest
  return ((double)total) / (double)units;
}

static int ring_used(pool *p)
{
  return p != NULL && (p->hd.free_list.alloc_count != 0 || p->hd.next != p);
}

/* TODO: thread-safe; simplify */
static int boxroot_used()
{
  if (ring_used(pools[0]->old)
      || ring_used(pools[0]->young)
      || ring_used(pools[0]->current))
    return 1;
  for (int i = 1; i < Num_domains; i++) {
    if (pools[i] != NULL) return 1;
  }
  if (ring_used(pools[Orphaned_id]->old)
      || ring_used(pools[Orphaned_id]->young)
      || ring_used(pools[Orphaned_id]->current))
    return 1;
  return 0;
}

void boxroot_print_stats()
{
  printf("minor collections: %'lld\n"
         "major collections (and others): %'lld\n",
         stats.minor_collections,
         stats.major_collections);

  if (stats.total_alloced_pools == 0) return;

  printf("POOL_LOG_SIZE: %d (%'lld KiB, %'d roots/pool)\n"
         "DEBUG: %d\n"
         "OCAML_MULTICORE: %d\n"
         "BOXROOT_USE_MUTEX: %d\n"
         "WITH_EXPECT: 1\n",
         (int)POOL_LOG_SIZE, kib_of_pools(1, 1), (int)POOL_ROOTS_CAPACITY,
         (int)DEBUG, (int)OCAML_MULTICORE, (int)BOXROOT_USE_MUTEX);

  printf("total allocated pools: %'lld (%'lld MiB)\n"
         "peak allocated pools: %'lld (%'lld MiB)\n"
         "total emptied pools: %'lld (%'lld MiB)\n"
         "total freed pools: %'lld (%'lld MiB)\n",
         stats.total_alloced_pools,
         kib_of_pools(stats.total_alloced_pools, 2),
         stats.peak_pools,
         kib_of_pools(stats.peak_pools, 2),
         stats.total_emptied_pools,
         kib_of_pools(stats.total_emptied_pools, 2),
         stats.total_freed_pools,
         kib_of_pools(stats.total_freed_pools, 2));

  double scanning_work_minor =
    average(stats.total_scanning_work_minor, stats.minor_collections);
  double scanning_work_major =
    average(stats.total_scanning_work_major, stats.major_collections);
  long long total_scanning_work =
    stats.total_scanning_work_minor + stats.total_scanning_work_major;
#if DEBUG
  double young_hits_gen_pct =
    average(stats.young_hit_gen * 100, stats.total_scanning_work_major);
#endif
  double young_hits_young_pct =
    average(stats.young_hit_young * 100, stats.total_scanning_work_minor);

  printf("work per minor: %'.0f\n"
         "work per major: %'.0f\n"
         "total scanning work: %'lld (%'lld minor, %'lld major)\n"
#if DEBUG
         "young hits (non-minor collection): %.2f%%\n"
#endif
         "young hits (minor collection): %.2f%%\n",
         scanning_work_minor,
         scanning_work_major,
         total_scanning_work, stats.total_scanning_work_minor, stats.total_scanning_work_major,
#if DEBUG
         young_hits_gen_pct,
#endif
         young_hits_young_pct);

#if defined(POSIX_CLOCK)
  double time_per_minor =
    average(stats.total_minor_time, stats.minor_collections) / 1000;
  double time_per_major =
    average(stats.total_major_time, stats.major_collections) / 1000;

  printf("average time per minor: %'.3fµs\n"
         "average time per major: %'.3fµs\n"
         "peak time per minor: %'.3fµs\n"
         "peak time per major: %'.3fµs\n",
         time_per_minor,
         time_per_major,
         ((double)stats.peak_minor_time) / 1000,
         ((double)stats.peak_major_time) / 1000);
#endif

  double ring_operations_per_pool =
    average(stats.ring_operations, stats.total_alloced_pools);

  printf("total ring operations: %'lld\n"
         "ring operations per pool: %.2f\n",
         stats.ring_operations,
         ring_operations_per_pool);

#if DEBUG
  long long total_create = stats.total_create_young + stats.total_create_old;
  long long total_delete = stats.total_delete_young + stats.total_delete_old;
  double create_young_pct =
    average(stats.total_create_young * 100, total_create);
  double delete_young_pct =
    average(stats.total_delete_young * 100, total_delete);

  printf("total created: %'lld (%.2f%% young)\n"
         "total deleted: %'lld (%.2f%% young)\n"
         "total modified: %'lld\n",
         total_create, create_young_pct,
         total_delete, delete_young_pct,
         stats.total_modify);

  printf("get_pool_header: %'lld\n"
         "is_pool_member: %'lld\n"
         "is_empty_free_list: %'lld\n",
         stats.get_pool_header,
         stats.is_pool_member,
         stats.is_empty_free_list);
#endif
}

/* }}} */

/* {{{ Hook setup */

static void scanning_callback(scanning_action action, int only_young,
                              void *data)
{
  if (status != RUNNING) return;
  int in_minor_collection = boxroot_in_minor_collection();
  if (in_minor_collection) incr(&stats.minor_collections);
  else incr(&stats.major_collections);
  int dom_id = Domain_id;
  if (pools[dom_id] == NULL) return; /* synchronised by domain lock */
  acquire_pool_rings(dom_id);
  // If no boxroot has been allocated, then scan_roots should not have
  // any noticeable cost. For experimental purposes, since this hook
  // is also used for other the statistics of other implementations,
  // we further make sure of this with an extra test, by avoiding
  // calling scan_roots if it has only just been initialised.
  if (boxroot_used()) {
#if !OCAML_MULTICORE
    boxroot_check_thread_hooks();
#endif
    long long start = time_counter();
    scan_roots(action, only_young, data, dom_id);
    long long duration = time_counter() - start;
    stat_t *total = in_minor_collection ? &stats.total_minor_time : &stats.total_major_time;
    stat_t *peak = in_minor_collection ? &stats.peak_minor_time : &stats.peak_major_time;
    *total += duration;
    if (duration > *peak) *peak = duration; // racy, but whatever
  }
  release_pool_rings(dom_id);
}

/* Handle orphaning of domain-local pools */
static void domain_termination_callback()
{
  DEBUGassert(OCAML_MULTICORE == 1);
  int dom_id = Domain_id;
  orphan_pools(dom_id);
}

/* Used for initialization/teardown */
static mutex_t init_mutex = BOXROOT_MUTEX_INITIALIZER;

int boxroot_setup()
{
  boxroot_mutex_lock(&init_mutex);
  if (status != NOT_SETUP) return 0;
  assert_domain_lock_held(Domain_id);
  boxroot_setup_hooks(&scanning_callback, &domain_termination_callback);
  /* Domain 0 can be accessed without going through acquire_pool_rings
     on OCaml 4 without mutex, so we need to initialize it right away. */
  init_pool_rings(0);
  init_pool_rings(Orphaned_id);
  // we are done
  status = RUNNING;
  boxroot_mutex_unlock(&init_mutex);
  return 1;
}

void boxroot_teardown()
{
  boxroot_mutex_lock(&init_mutex);
  if (status != RUNNING) return;
  status = FREED;
  for (int i = 0; i < Num_domains; i++) {
    pool_rings *ps = pools[i];
    if (ps == NULL) continue;
    free_pool_rings(ps);
    free(ps);
  }
  boxroot_mutex_unlock(&init_mutex);
}

/* }}} */

/* {{{ */
/* }}} */
