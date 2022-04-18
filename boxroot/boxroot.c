/* {{{ Includes */

// This is emacs folding-mode

#include <assert.h>
#include <limits.h>
#if BOXROOT_USE_MUTEX
#include <pthread.h>
#endif
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
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
static struct {
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
} pools;

static boxroot_fl empty_fl = { (void *)&empty_fl, -1 };

boxroot_fl *boxroot_current_fl = &empty_fl;

static pool ** const global_rings[] =
  { &pools.old, &pools.young, &pools.current, &pools.free, NULL };

static const class global_ring_classes[] =
  { OLD, YOUNG, YOUNG, UNTRACKED };

/* Iterate on all global rings.
   [global_ring]: a variable of type [pool**].
   [cl]: a variable of type [class].
   [action]: an expression that can refer to global_ring and cl.
*/
#define FOREACH_GLOBAL_RING(global_ring, cl, action) do {               \
    pool ** const *b__st = &global_rings[0];                            \
    for (pool ** const *b__i = b__st; *b__i != NULL; b__i++) {          \
      pool **global_ring = *b__i;                                       \
      class cl = global_ring_classes[b__i - b__st];                     \
      action;                                                           \
      (void)cl;                                                         \
    }                                                                   \
  } while (0)

#if BOXROOT_USE_MUTEX
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#define CRITICAL_SECTION_BEGIN() pthread_mutex_lock(&mutex)
#define CRITICAL_SECTION_END() pthread_mutex_unlock(&mutex)
#else
#define CRITICAL_SECTION_BEGIN()
#define CRITICAL_SECTION_END()
#endif

struct stats {
  int minor_collections;
  int major_collections;
  intnat total_create_young;
  intnat total_create_old;
  intnat total_delete_young;
  intnat total_delete_old;
  intnat total_modify;
  long long total_scanning_work_minor;
  long long total_scanning_work_major;
  int64_t total_minor_time;
  int64_t total_major_time;
  int64_t peak_minor_time;
  int64_t peak_major_time;
  int total_alloced_pools;
  int total_freed_pools;
  int live_pools; // number of tracked pools
  int peak_pools; // max live pools at any time
  int ring_operations; // Number of times hd.next is mutated
  long long young_hit; // number of times a young value was encountered
                       // during scanning
  long long get_pool_header; // number of times get_pool_header was called
  long long is_pool_member; // number of times is_pool_member was called
  long long is_empty_free_list; // number of times is_empty_free_list was called
};

static struct stats stats;

/* }}} */

/* {{{ Tests in the hot path */

// hot path
static inline pool * get_pool_header(slot *s)
{
  if (DEBUG) ++stats.get_pool_header;
  return Get_pool_header(s);
}

// Return true iff v shares the same msbs as p and is not an
// immediate.
// hot path
static inline int is_pool_member(slot v, pool *p)
{
  if (DEBUG) ++stats.is_pool_member;
  return (uintptr_t)p == ((uintptr_t)v & ~((uintptr_t)POOL_SIZE - 2));
}

// hot path
static inline int is_empty_free_list(slot *v, pool *p)
{
  if (DEBUG) ++stats.is_empty_free_list;
  return (v == (slot *)p);
}

/* }}} */

/* {{{ Ring operations */

static inline void ring_link(pool *p, pool *q)
{
  p->hd.next = q;
  q->hd.prev = p;
  ++stats.ring_operations;
}

// insert the ring [source] at the back of [*target].
static inline void ring_push_back(pool *source, pool **target)
{
  DEBUGassert(source != NULL);
  DEBUGassert(source->hd.prev == source && source->hd.next == source);
  DEBUGassert(source != *target);
  if (*target == NULL) {
    *target = source;
    if (DEBUG) {
      FOREACH_GLOBAL_RING(global, cl, {
          assert(target != global || source->hd.class == cl);
        });
    }
  } else {
    DEBUGassert((*target)->hd.class == source->hd.class);
    pool *target_last = (*target)->hd.prev;
    pool *source_last = source->hd.prev;
    ring_link(target_last, source);
    ring_link(source_last, *target);
  }
}

static inline void ring_push_front(pool *source, pool **target)
{
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
  pool *p = alloc_uninitialised_pool(POOL_SIZE);
  if (p == NULL) return NULL;
  ++stats.total_alloced_pools;
  ring_link(p, p);
  p->hd.free_list.next = NULL;
  p->hd.free_list.alloc_count = 0;
  p->hd.class = UNTRACKED;
  return p;
}

// the empty free-list for a pool p is denoted by a pointer to the pool itself
// (NULL could be a valid value for an element slot)
static inline slot empty_free_list(pool *p) {
  return (slot)p;
}

static inline int is_full_pool(pool *p)
{
  return is_empty_free_list(p->hd.free_list.next, p);
}

static pool * get_empty_pool()
{
  ++stats.live_pools;
  if (stats.live_pools > stats.peak_pools) stats.peak_pools = stats.live_pools;
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
      free_pool(p);
  }
}

static void free_all_pools()
{
  FOREACH_GLOBAL_RING(global, cl, {
    free_pool_ring(global);
  });
}

/* }}} */

/* {{{ Pool class management */

static inline int is_not_too_full(pool *p)
{
  return p->hd.free_list.alloc_count <= (int)(DEALLOC_THRESHOLD / sizeof(slot));
}

static void set_current_pool(pool *p)
{
  DEBUGassert(pools.current == NULL);
  pools.current = p;
  if (p != NULL) {
    p->hd.class = YOUNG;
    boxroot_current_fl = &p->hd.free_list;
  } else {
    boxroot_current_fl = &empty_fl;
  }
}

/* Move not-too-full pools to the front; move empty pools to the free
   ring. */
static void try_demote_pool(pool *p)
{
  DEBUGassert(p->hd.class != UNTRACKED);
  if (p == pools.current || !is_not_too_full(p)) return;
  pool **source = (p->hd.class == OLD) ? &pools.old : &pools.young;
  pool **target;
  if (p->hd.free_list.alloc_count == 0) {
    target = &pools.free;
    p->hd.class = UNTRACKED;
  } else {
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
  /* When pools empty themselves, they are pushed to the front.
     If the first one is full, then none are empty enough. */
  if (*target == NULL || is_full_pool(*target)) return NULL;
  return ring_pop(target);
}

/* Find an available pool and set it as current. Return NULL if none
   was found and the allocation of a new one failed. */
static pool * find_available_pool()
{
  pool *p = pop_available(&pools.young);
  if (p == NULL && pools.old != NULL && is_not_too_full(pools.old))
    p = pop_available(&pools.old);
  if (p == NULL) p = pop_available(&pools.free);
  if (p == NULL) p = get_empty_pool();
  DEBUGassert(pools.current == NULL);
  set_current_pool(p);
  return p;
}

static void reclassify_pool(pool **source, class cl)
{
  assert(*source != NULL);
  pool *p = ring_pop(source);
  pool **target = (cl == YOUNG) ? &pools.young : &pools.old;
  p->hd.class = cl;
  if (is_not_too_full(p)) {
    ring_push_front(p, target);
  } else {
    ring_push_back(p, target);
  }
}

static void promote_young_pools()
{
  // Promote full pools
  while (pools.young != NULL) {
    reclassify_pool(&pools.young, OLD);
  }
  // Heuristic: if a young pool has just been allocated, it is better
  // if it is the first one to be considered next time a young boxroot
  // allocation takes place. So we promote the current pool last.
  if (pools.current != NULL) {
    reclassify_pool(&pools.current, OLD);
    set_current_pool(NULL);
  }
  // A program that does not use any boxroot should not have to pay
  // the cost of scanning any pool.
  assert(pools.young == NULL && pools.current == NULL);
}

/* }}} */

/* {{{ Allocation, deallocation */

static int setup;

// Set an available pool as current and allocate from it.
boxroot boxroot_alloc_slot_slow(value init)
{
  // We might be here because boxroot is not setup.
  if (!setup) {
    fprintf(stderr, "boxroot is not setup\n");
    return NULL;
  }
  if (pools.current != NULL) {
    DEBUGassert(is_full_pool(pools.current));
    reclassify_pool(&pools.current, YOUNG);
  }
  // TODO Latency: bound the number of roots allocated between each
  // minor collection by scheduling a minor collection?
  pool *p = find_available_pool();
  if (p == NULL) return NULL;
  assert(!is_full_pool(p));
  return boxroot_alloc_slot(init);
}

/* }}} */

/* {{{ Boxroot API implementation */

extern inline value boxroot_get(boxroot root);
extern inline value const * boxroot_get_ref(boxroot root);

extern inline boxroot boxroot_alloc_slot(value init);

boxroot boxroot_create_debug(value init)
{
  CRITICAL_SECTION_BEGIN();
  if (DEBUG) {
    if (Is_block(init) && Is_young(init)) ++stats.total_create_young;
    else ++stats.total_create_old;
  }
  boxroot br = boxroot_alloc_slot(init);
  CRITICAL_SECTION_END();
  return br;
}

extern inline boxroot boxroot_create(value init);

extern inline void boxroot_free_slot(boxroot root);

void boxroot_delete_debug(boxroot root)
{
  CRITICAL_SECTION_BEGIN();
  DEBUGassert(root != NULL);
  if (DEBUG) {
    value v = boxroot_get(root);
    if (Is_block(v) && Is_young(v)) ++stats.total_delete_young;
    else ++stats.total_delete_old;
  }
  boxroot_free_slot(root);
  CRITICAL_SECTION_END();
}

extern inline void boxroot_delete(boxroot root);

static void boxroot_reallocate(boxroot *root, value new_value)
{
  boxroot new = boxroot_alloc_slot(new_value);
  if (LIKELY(new != NULL)) {
    boxroot_free_slot(*root);
    *root = new;
  } else {
    // Better not fail in boxroot_modify. Expensive but fail-safe:
    // demote its pool into the young pools.
    pool *p = get_pool_header((slot *)*root);
    DEBUGassert(p->hd.class == OLD);
    pool **source = (p == pools.old) ? &pools.old : &p;
    reclassify_pool(source, YOUNG);
    **((value **)root) = new_value;
  }
}

// hot path
void boxroot_modify(boxroot *root, value new_value)
{
  CRITICAL_SECTION_BEGIN();
  slot *s = (slot *)*root;
  DEBUGassert(s);
  if (DEBUG) ++stats.total_modify;
  pool *p = get_pool_header(s);
  if (LIKELY(p->hd.class == YOUNG
             || !Is_block(new_value)
             || !Is_young(new_value))) {
    *(value *)s = new_value;
  } else {
    // We need to reallocate, but this reallocation happens at most once
    // between two minor collections.
    boxroot_reallocate(root, new_value);
  }
  CRITICAL_SECTION_END();
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
      if (pl->hd.class != YOUNG) assert(!Is_block(v) || !Is_young(v));
      ++alloc_count;
    }
  }
  assert(alloc_count == pl->hd.free_list.alloc_count);
}

static void validate_all_pools()
{
  FOREACH_GLOBAL_RING(global, cl, {
      pool *start_pool = *global;
      if (start_pool == NULL) continue;
      pool *p = start_pool;
      do {
        assert(p->hd.class == cl);
        validate_pool(p);
        assert(p->hd.next != NULL);
        assert(p->hd.next->hd.prev == p);
        assert(p->hd.prev != NULL);
        assert(p->hd.prev->hd.next == p);
        p = p->hd.next;
      } while (p != start_pool);
    });
}

// returns the amount of work done
static int scan_pool_gen(scanning_action action, void *data, pool *pl)
{
  int allocs_to_find = pl->hd.free_list.alloc_count;
  slot *current = pl->roots;
  while (allocs_to_find) {
    // hot path
    slot s = *current;
    if (LIKELY((!is_pool_member(s, pl)))) {
      --allocs_to_find;
      value v = (value)s;
      if (DEBUG && Is_block(v) && Is_young(v)) ++stats.young_hit;
      CALL_GC_ACTION(action, data, v, (value *)current);
    }
    ++current;
  }
  return current - pl->roots;
}

/* Benchmark results for minor scanning:
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
  (void)action;
#endif
  slot *start = pl->roots;
  slot *end = start + POOL_ROOTS_CAPACITY;
  for (slot *i = start; i < end; i++) {
    value v = (value)*i;
    // If v falls within the young range it is likely that it is a block
    if ((uintnat)v - young_start <= young_range && Is_block(v)) {
      ++stats.young_hit;
      CALL_GC_ACTION(action, data, v, (value *)i);
    }
  }
  return POOL_ROOTS_CAPACITY;
}

static int scan_pool(scanning_action action, int only_young, void *data,
                     pool *pl)
{
  if (only_young)
    return scan_pool_young(action, data, pl);
  else
    return scan_pool_gen(action, data, pl);
}

static int scan_pools(scanning_action action, int only_young, void *data)
{
  int work = 0;
  FOREACH_GLOBAL_RING(global, cl, {
      if (cl == UNTRACKED || (boxroot_in_minor_collection()
                                 && cl == OLD))
        continue;
      pool *start_pool = *global;
      if (start_pool == NULL) continue;
      pool *p = start_pool;
      do {
        work += scan_pool(action, only_young, data, p);
        p = p->hd.next;
      } while (p != start_pool);
    });
  return work;
}

static void scan_roots(scanning_action action, int only_young, void *data)
{
  if (DEBUG) validate_all_pools();
  int work = scan_pools(action, only_young, data);
  if (boxroot_in_minor_collection()) {
    promote_young_pools();
    stats.total_scanning_work_minor += work;
  } else {
    stats.total_scanning_work_major += work;
    free_pool_ring(&pools.free);
  }
  if (DEBUG) validate_all_pools();
}

/* }}} */

/* {{{ Statistics */

static int64_t time_counter(void)
{
#if defined(POSIX_CLOCK)
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * (int64_t)1000000000 + (int64_t)t.tv_nsec;
#else
  return 0;
#endif
}

// 1=KiB, 2=MiB
static int kib_of_pools(int count, int unit)
{
  int log_per_pool = POOL_LOG_SIZE - unit * 10;
  if (log_per_pool >= 0) return count << log_per_pool;
  /* log_per_pool < 0) */
  return count >> -log_per_pool;
}

static int average(long long total_work, int nb_collections)
{
  if (nb_collections <= 0) return -1;
  // round to nearest
  return (total_work + (nb_collections / 2)) / nb_collections;
}

static int boxroot_used()
{
  FOREACH_GLOBAL_RING (global, cl, {
      if (cl == UNTRACKED) continue;
      pool *p = *global;
      if (p != NULL && (p->hd.free_list.alloc_count != 0 || p->hd.next != p)) {
        return 1;
      }
    });
  return 0;
}

void boxroot_print_stats()
{
  printf("minor collections: %d\n"
         "major collections (and others): %d\n",
         stats.minor_collections,
         stats.major_collections);

  int scanning_work_minor = average(stats.total_scanning_work_minor, stats.minor_collections);
  int scanning_work_major = average(stats.total_scanning_work_major, stats.major_collections);
  long long total_scanning_work = stats.total_scanning_work_minor + stats.total_scanning_work_major;
  int ring_operations_per_pool = average(stats.ring_operations, stats.total_alloced_pools);

  if (stats.total_alloced_pools == 0) return;

  int64_t time_per_minor =
      stats.minor_collections ? stats.total_minor_time / stats.minor_collections : 0;
  int64_t time_per_major =
      stats.major_collections ? stats.total_major_time / stats.major_collections : 0;

  printf("POOL_LOG_SIZE: %d (%'d KiB, %'d roots/pool)\n"
         "DEBUG: %d\n"
         "OCAML_MULTICORE: %d\n"
         "BOXROOT_USE_MUTEX: %d\n"
         "WITH_EXPECT: 1\n",
         (int)POOL_LOG_SIZE, kib_of_pools((int)1, 1), (int)POOL_ROOTS_CAPACITY,
         (int)DEBUG, (int)OCAML_MULTICORE, (int)BOXROOT_USE_MUTEX);

  printf("total allocated pool: %'d (%'d MiB)\n"
         "peak allocated pools: %'d (%'d MiB)\n"
         "total freed pool: %'d (%'d MiB)\n",
         stats.total_alloced_pools,
         kib_of_pools(stats.total_alloced_pools, 2),
         stats.peak_pools,
         kib_of_pools(stats.peak_pools, 2),
         stats.total_freed_pools,
         kib_of_pools(stats.total_freed_pools, 2));

  int young_hits_pct = stats.total_scanning_work_minor ?
    (stats.young_hit * 100) / stats.total_scanning_work_minor
    : -1;

  printf("work per minor: %'d\n"
         "work per major: %'d\n"
         "total scanning work: %'lld (%'lld minor, %'lld major)\n"
         "young hits: %d%%\n",
         scanning_work_minor,
         scanning_work_major,
         total_scanning_work, stats.total_scanning_work_minor, stats.total_scanning_work_major,
         young_hits_pct);

#if defined(POSIX_CLOCK)
  printf("average time per minor: %'lldns\n"
         "average time per major: %'lldns\n"
         "peak time per minor: %'lldns\n"
         "peak time per major: %'lldns\n",
         (long long)time_per_minor,
         (long long)time_per_major,
         (long long)stats.peak_minor_time,
         (long long)stats.peak_major_time);
#endif

  printf("total ring operations: %'d\n"
         "ring operations per pool: %'d\n",
         stats.ring_operations,
         ring_operations_per_pool);

#if DEBUG != 0
  intnat total_create = stats.total_create_young + stats.total_create_old;
  intnat total_delete = stats.total_delete_young + stats.total_delete_old;
  intnat create_young_pct = total_create ?
    (stats.total_create_young * 100 / total_create) : -1;
  intnat delete_young_pct = total_delete ?
    (stats.total_delete_young * 100 / total_delete) : -1;

  printf("total created: %'ld (%ld%% young)\n"
         "total deleted: %'ld (%ld%% young)\n"
         "total modified: %'ld\n",
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

static int setup = 0;

static void scanning_callback(scanning_action action, int only_young,
                              void *data)
{
  CRITICAL_SECTION_BEGIN();
  if (!setup) {
    CRITICAL_SECTION_END();
    return;
  }
  int in_minor_collection = boxroot_in_minor_collection();
  if (in_minor_collection) ++stats.minor_collections;
  else ++stats.major_collections;
  // If no boxroot has been allocated, then scan_roots should not have
  // any noticeable cost. For experimental purposes, since this hook
  // is also used for other the statistics of other implementations,
  // we further make sure of this with an extra test, by avoiding
  // calling scan_roots if it has only just been initialised.
  if (boxroot_used()) {
    int64_t start = time_counter();
    scan_roots(action, only_young, data);
    int64_t duration = time_counter() - start;
    int64_t *total = in_minor_collection ? &stats.total_minor_time : &stats.total_major_time;
    int64_t *peak = in_minor_collection ? &stats.peak_minor_time : &stats.peak_major_time;
    *total += duration;
    if (duration > *peak) *peak = duration;
  }
  CRITICAL_SECTION_END();
}

// Must be called to set the hook before using boxroot
int boxroot_setup()
{
  CRITICAL_SECTION_BEGIN();
  if (setup) {
    CRITICAL_SECTION_END();
    return 0;
  }
  // initialise globals
  struct stats empty_stats = {0};
  stats = empty_stats;
  FOREACH_GLOBAL_RING(global, cl, { *global = NULL; });
  boxroot_setup_hooks(&scanning_callback);
  // we are done
  setup = 1;
  CRITICAL_SECTION_END();
  return 1;
}

// This can only be called at OCaml shutdown
void boxroot_teardown()
{
  CRITICAL_SECTION_BEGIN();
  if (!setup) {
    CRITICAL_SECTION_END();
    return;
  }
  setup = 0;
  free_all_pools();
  CRITICAL_SECTION_END();
}

/* }}} */

/* {{{ */
/* }}} */
