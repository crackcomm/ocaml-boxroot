/* {{{ Includes */

// This is emacs folding-mode

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAML_NAME_SPACE
#define CAML_INTERNALS

#include "boxroot.h"
#include <caml/roots.h>
#include <caml/minor_gc.h>
#include <caml/major_gc.h>
#include <caml/compact.h>
#include <caml/fail.h>

/* }}} */

/* {{{ Options */

#define POOL_LOG_SIZE 13 // 12 = 4KB

#define NUM_THRESHOLD_LOG 5 // 32
 /* Old pools become candidate for young allocation below
    LOW_COUNT_THRESHOLD / 2^NUM_THRESHOLD_LOG occupancy. This tries to
    guarantee that minor scanning hits a good proportion of young
    values.
    Recommended: 16. */
#define LOW_COUNT_THRESHOLD 16
 /* Pools become candidate for allocation below
    HIGH_COUNT_THRESHOLD / 2^NUM_THRESHOLD_LOG occupancy.
    (0 < LOW_COUNT_THRESHOLD < HIGH_COUNT_THRESHOLD < 31.)
    Recommended: 31.*/
#define HIGH_COUNT_THRESHOLD 31
/* Print statistics on teardown from OCaml? */
#ifdef BOXROOT_STATS
#define PRINT_STATS 1
#else
#define PRINT_STATS 0
#endif
/* Check integrity of pool structure after each scan, and print
   additional statistics? (slow) */
#define DEBUG 0
/* Allocate with mmap? */
#define USE_MMAP 0
/* Advise to use transparent huge pages? (Linux)

   Little effect in my experiments. This is to investigate further:
   indeed the options "thp:always,metadata_thp:auto" for jemalloc
   consistently brings it faster than ocaml-ephemeral. FTR:
   `MALLOC_CONF="thp:always,metadata_thp:auto"                 \
   LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2       \
   make run`. (probably due to the malloc in value_list_functor.h)
*/
#define USE_MADV_HUGEPAGE 1
/* Whether to pre-allocate several pools at once. Free pools are put
   aside and re-used instead of being immediately freed. Does not
   support memory reclamation yet.
   + Visible effect for large pool sizes (independently of superblock
     size) with glibc malloc. Probably by avoiding the cost of memory
     reclamation. Note: this is fair: it is enough to reclaim memory
     at Gc.compact like the OCaml Gc. Not observed with jemalloc.
   - Slower than aligned_alloc for small pool sizes (independently of
     superblock sizes?). */
#define USE_SUPERBLOCK 1
#define SUPERBLOCK_LOG_SIZE 21 // 21 = 2MB (a huge page)
/* Defragment during compaction?
   + Better cache locality for successive allocations after lots of
     deallocations.
   + Improves early exit during scanning after lots of deallocations.
   TODO:
    - Implement defrag on demotion
*/
#define DEFRAG 0
/* Use __builtin_expect in hot paths? (suggested by looking at the
   generated assembly in godbolt).
   Invert is sometimes faster (~5%) => probably shows effect of code
   (mis)alignment.
   0 = off
   1 = on
   2 = invert
 */
#define WITH_EXPECT 1
/* A potentially faster is_young test. */
#define FAST_IS_YOUNG 0

/* }}} */

/* {{{ Setup */

#if PRINT_STATS != 0
#include <locale.h>
#endif

// for MADV_HUGEPAGE
#define HUGEPAGE_LOG_SIZE 21 // 2MB on x86_64 Linux
#if USE_MADV_HUGEPAGE != 0
  #include <sys/mman.h>
#endif

#if WITH_EXPECT == 0
  #define LIKELY(a) (a)
  #define UNLIKELY(a) (a)
#elif WITH_EXPECT == 1
  #define LIKELY(a) __builtin_expect(!!(a),1)
  #define UNLIKELY(a) __builtin_expect(!!(a),0)
#elif WITH_EXPECT == 2
  #define LIKELY(a) __builtin_expect(!!(a),0)
  #define UNLIKELY(a) __builtin_expect(!!(a),1)
#endif

#define CHUNK_LOG_SIZE (USE_SUPERBLOCK ? SUPERBLOCK_LOG_SIZE : POOL_LOG_SIZE)

static_assert(CHUNK_LOG_SIZE >= POOL_LOG_SIZE,
              "chunk size smaller than pool size");
static_assert(!USE_MADV_HUGEPAGE || CHUNK_LOG_SIZE >= HUGEPAGE_LOG_SIZE,
              "chunk size smaller than a huge page");

#define POOL_SIZE ((size_t)1 << POOL_LOG_SIZE)
#define CHUNK_SIZE ((size_t)1 << CHUNK_LOG_SIZE)
#define CHUNK_ALIGNMENT                                         \
  ((USE_MADV_HUGEPAGE && POOL_LOG_SIZE < HUGEPAGE_LOG_SIZE) ?   \
   ((size_t)1 << HUGEPAGE_LOG_SIZE) : POOL_SIZE)
#define POOLS_PER_CHUNK (CHUNK_SIZE / POOL_SIZE)

/* }}} */

/* {{{ Data types */

typedef enum class {
  YOUNG,
  OLD,
  UNTRACKED
} class;

typedef void * slot;

struct header {
  struct pool *prev;
  struct pool *next;
  slot *free_list;
  int alloc_count;
  class class;
};

static_assert(POOL_SIZE / sizeof(slot) <= INT_MAX, "pool size too large");

#define POOL_ROOTS_CAPACITY                                 \
  ((int)((POOL_SIZE - sizeof(struct header)) / sizeof(slot) - 1))
/* &pool->roots[POOL_ROOTS_CAPACITY] can end up as a placeholder value
   in the freelist to denote the last element of the freelist,
   starting from after releasing from a full pool for the first
   time. To ensure that this value is recognised by the test
   [is_pool_member(v, pool)], we subtract one from the capacity. */

typedef struct pool {
  struct header hd;
  /* Occupied slots are OCaml non-immediate values. Unoccupied slots
     are a pointer to the next slot in the free list, or to the end of
     the array denoting the end of the free list. */
  slot roots[POOL_ROOTS_CAPACITY];
  uintptr_t end;// unused
} pool;

static_assert(sizeof(pool) == POOL_SIZE, "bad pool size");

/* }}} */

/* {{{ Globals */

/* Global pool rings. */
static struct {
/* Pools of old values: contains only roots pointing to the major
   heap. Scanned at the start of major collection. */
  /* Full or almost. Not considered for allocation. */
  pool *old_full;
  /* Never NULL. */
  pool *old_available;
  /* Pools with lots of available space, considered in priority for
     recycling into a young pool.*/
  pool *old_low;

/* Pools of young values: contains roots pointing to the major or to
   the minor heap. Scanned at the start of minor and major
   collection. */
  /* Never NULL. */
  pool *young_available;
  /* Full or almost. Not considered for allocation. */
  pool *young_full;

/* Pools containing no root: not scanned.*/
  /* Initialized */
  pool *free;
  /* Unitialised */
  pool *uninitialised;
} pools; // zero-initialized

static pool **global_rings[] =
  { &pools.old_full, &pools.old_available, &pools.old_low,
    &pools.young_available, &pools.young_full, &pools.free,
    &pools.uninitialised, NULL };

static class global_ring_classes[] = { OLD, OLD, OLD,
                                       YOUNG, YOUNG,
                                       UNTRACKED, UNTRACKED };

/* Iterate on all global rings.
   [global_ring]: a variable of type [pool**].
   [cl]: a variable of type [class].
   [action]: an expression that can refer to global_ring and cl.
*/
#define FOREACH_GLOBAL_RING(global_ring, cl, action) do {               \
    pool ***b__st = &global_rings[0];                                   \
    for (pool ***b__i = b__st; *b__i != NULL; b__i++) {                 \
      pool **global_ring = *b__i;                                       \
      class cl = global_ring_classes[b__i - b__st];                     \
      action;                                                           \
    }                                                                   \
  } while (0)

static struct {
  int minor_collections;
  int major_collections;
  int total_create;
  int total_delete;
  int total_modify;
  long long total_scanning_work_minor;
  long long total_scanning_work_major;
  int total_alloced_chunks;
  int total_alloced_pools;
  int total_freed_pools;
  int live_pools; // number of tracked pools
  int peak_pools; // max live pools at any time
  int ring_operations; // Number of times hd.next is mutated
  int defrag_sort;
  long long is_young; // number of times is_young was called
  long long young_hit; // number of times a young value was encountered
                 // during scanning
  long long get_pool_header; // number of times get_pool_header was called
  long long is_pool_member; // number of times is_pool_member was called
  long long is_last_elem; // number of times is_last_elem was called
} stats; // zero-initialized

/* }}} */

/* {{{ Tests in the hot path */

// hot path
static inline pool * get_pool_header(slot v)
{
  if (DEBUG) ++stats.get_pool_header;
  return (pool *)((uintptr_t)v & ~((uintptr_t)POOL_SIZE - 1));
}

// hot path
// Return true iff v shares the same msbs as p and is not an
// immediate.
static inline int is_pool_member_nostats(slot v, pool *p)
{
  /* 0bxxxxx0000001 */
  return (uintptr_t)p == ((uintptr_t)v & ~((uintptr_t)POOL_SIZE - 2));
}
static inline int is_pool_member(slot v, pool *p)
{
  if (DEBUG) ++stats.is_pool_member;
  return is_pool_member_nostats(v, p);
}

// hot path
static inline int is_last_elem(slot *v)
{
  if (DEBUG) ++stats.is_last_elem;
  return ((uintptr_t)(v + 1) & (POOL_SIZE - 1)) == 0;
}

// hot path
static inline int is_young(value v)
{
  if (DEBUG) ++stats.is_young;
#if FAST_IS_YOUNG != 0
  /* a < x <= b  <=>  (unsigned)(b-x) < b-a  (from Hacker's Delight)
     Note that b cannot be the address of a value because the header would be in b-1. */
  return (unsigned int)((uintptr_t)caml_young_end - (uintptr_t)v) < (unsigned int)(
    (uintptr_t)caml_young_end - (uintptr_t)caml_young_start);
#else
  return Is_young(v);
#endif
}

/* }}} */

/* {{{ Platform-specific allocation */

#ifdef __APPLE__
static void *aligned_alloc(size_t alignment, size_t size) {
  void *memptr = NULL;
  posix_memalign(&memptr, alignment, size);
  return memptr;
}
#endif

static void * alloc_chunk()
{
  void *p = NULL;
  if (USE_MMAP) {
    uintptr_t bitmask = ((uintptr_t)CHUNK_ALIGNMENT) - 1;
    p = mmap(0, CHUNK_SIZE + CHUNK_ALIGNMENT, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = NULL;
    // align p
    p = (void *)(((uintptr_t)p + bitmask) & ~bitmask);
    // TODO: release unused portions
  } else {
    p = aligned_alloc(CHUNK_ALIGNMENT, CHUNK_SIZE); // TODO: not portable
  }
  if (p == NULL) return NULL;
#if USE_MADV_HUGEPAGE != 0
  madvise(p, CHUNK_SIZE, MADV_HUGEPAGE);
#endif
  ++stats.total_alloced_chunks;
  return p;
}

static void free_chunk(void *p)
{
  if (USE_MMAP) {
    // TODO: not implemented
    //munmap(p, CHUNK_SIZE);
  } else {
    free(p);
  }
}

/* }}} */

/* {{{ Ring operations */

static void ring_init(pool *p)
{
  p->hd.next = p;
  p->hd.prev = p;
  ++stats.ring_operations;
}

static void validate_pool(pool*);

// insert the ring [source] in front of [*target]
static void ring_concat(pool *source, pool **target)
{
  if (source == NULL) return;
  pool *old = *target;
  if (old == NULL) {
    *target = source;
    if (DEBUG) {
      FOREACH_GLOBAL_RING(global, class, {
          assert(target != global || source->hd.class == class);
        });
    }
  } else {
    assert(old->hd.class == source->hd.class);
    pool *last = old->hd.prev;
    last->hd.next = source;
    source->hd.prev->hd.next = old;
    old->hd.prev = source->hd.prev;
    source->hd.prev = last;
    *target = source;
    stats.ring_operations += 2;
  }
}

// remove the first element from [*target] and return it
static pool * ring_pop(pool **target)
{
  pool *front = *target;
  assert(front);
  if (front->hd.next == front) {
    assert(front->hd.prev == front);
    *target = NULL;
    return front;
  }
  front->hd.prev->hd.next = front->hd.next;
  front->hd.next->hd.prev = front->hd.prev;
  ++stats.ring_operations;
  *target = front->hd.next;
  ring_init(front);
  return front;
}

/* }}} */

/* {{{ Pool management */

static pool * get_uninitialised_pool()
{
  if (pools.uninitialised != NULL) {
    return ring_pop(&pools.uninitialised);
  }
  pool *chunk = alloc_chunk();
  if (chunk == NULL) return NULL;
  for (pool *p = chunk + POOLS_PER_CHUNK - 1; p >= chunk; p--) {
    ring_init(p);
    p->hd.free_list = NULL;
    p->hd.alloc_count = 0;
    p->hd.class = UNTRACKED;
    ring_concat(p, &pools.uninitialised);
  }
  return ring_pop(&pools.uninitialised);
}

static pool * alloc_pool()
{
  ++stats.total_alloced_pools;
  ++stats.live_pools;
  if (stats.live_pools > stats.peak_pools)
    stats.peak_pools = stats.live_pools;
  pool *out = get_uninitialised_pool();

  if (out == NULL) return NULL;

  out->hd.free_list = out->roots;
  slot *end = &out->roots[POOL_ROOTS_CAPACITY];
  slot *s = out->roots;
  while (s < end) {
    slot *next = s + 1;
    *s = (slot)next;
    s = next;
  }

  return out;
}

static void pool_remove(pool *p)
{
  pool *old = ring_pop(&p);
  FOREACH_GLOBAL_RING(global, cl, {
      if (*global == old) *global = p;
    });
}

static int free_all_chunks(pool *start_pool)
{
  if (USE_SUPERBLOCK) {
    // TODO: not implemented
    return 0;
  }
  if (start_pool == NULL) return 0;
  int work = 0;
  pool *p = start_pool;
  do {
    pool *next = p->hd.next;
    free_chunk(p);
    p = next;
    work++;
    --stats.live_pools;
  } while (p != start_pool);
  return work;
}

/* }}} */

/* {{{ Pool class management */

// Find an available pool for the class; place it in front of the ring
// of available pools and return it. Return NULL if none was found and
// the allocation of a new one failed.
static pool * populate_pools(class class)
{
  assert(class != UNTRACKED);
  pool **target = (class == YOUNG) ?
    &pools.young_available : &pools.old_available;
  if (*target != NULL &&
      !is_last_elem((*target)->hd.free_list)) {
    return *target;
  }
  pool *new_pool = NULL;
  if (pools.old_low != NULL) {
    // YOUNG: We prefer to use an old pool which is not too full. We try to
    // guarantee a good young-to-old ratio during minor scanning.
    // OLD: We reserve the less full pools for re-use as young pools, but
    // we did what we could, so take a less full one anyway.
    new_pool = ring_pop(&pools.old_low);
    // Do not bother with quasi-full pools.
  } else if (pools.free != NULL) {
    new_pool = ring_pop(&pools.free);
  } else {
    // High time we allocate a pool.
    new_pool = alloc_pool();
  }
  if (new_pool == NULL) return NULL;
  new_pool->hd.class = class;
  ring_concat(new_pool, target);
  return new_pool;
}

#define NUM_THRESHOLD ((int)1 << NUM_THRESHOLD_LOG)
#define THRESHOLD_SIZE (POOL_SIZE / (NUM_THRESHOLD * sizeof(slot)))

static_assert(0 < LOW_COUNT_THRESHOLD, "");
static_assert(LOW_COUNT_THRESHOLD < HIGH_COUNT_THRESHOLD, "");
static_assert(HIGH_COUNT_THRESHOLD < NUM_THRESHOLD, "");
static_assert(1 + HIGH_COUNT_THRESHOLD * THRESHOLD_SIZE < POOL_ROOTS_CAPACITY,
              "HIGH_COUNT_THRESHOLD too high");

// hot path
static inline int is_alloc_threshold(int alloc_count)
{
  return (alloc_count & (THRESHOLD_SIZE - 1)) == 0;
}

typedef enum pool_class {
  EMPTY,
  LOW,
  HIGH,
  QUASI_FULL,
  NO_CHANGE
} pool_class;

static int get_threshold(int alloc_count)
{
  return 1 + (alloc_count - 1) / THRESHOLD_SIZE;
}

static pool_class pool_class_promote(pool *p)
{
  int threshold = get_threshold(p->hd.alloc_count);
  if (threshold == 0) return EMPTY;
  if (threshold <= LOW_COUNT_THRESHOLD) return LOW;
  if (threshold <= HIGH_COUNT_THRESHOLD) return HIGH;
  return QUASI_FULL;
}

static pool_class pool_class_demote(pool *p)
{
  assert(is_alloc_threshold(p->hd.alloc_count));
  int threshold = get_threshold(p->hd.alloc_count);
  if (threshold == 0) return EMPTY;
  if (threshold == LOW_COUNT_THRESHOLD) return LOW;
  if (threshold == HIGH_COUNT_THRESHOLD) return HIGH;
  return NO_CHANGE;
}

static void pool_reclassify(pool *p, pool_class new_pool_class)
{
  assert(new_pool_class != NO_CHANGE);
  assert(p->hd.next == p);
  class value_class = p->hd.class;
  assert((value_class == UNTRACKED) == (new_pool_class == EMPTY));
  int is_young = value_class == YOUNG;
  pool **target = NULL;
  switch (new_pool_class) {
  case EMPTY:
    assert(p->hd.alloc_count == 0);
    target = &pools.free;
    break;
  case LOW:
    target = is_young ? &pools.young_available : &pools.old_low;
    break;
  case HIGH:
    target = is_young ? &pools.young_available : &pools.old_available;
    break;
  case QUASI_FULL:
    target = is_young ? &pools.young_full : &pools.old_full;
    break;
  }
  // Add at the end instead of in front, since pools which have been
  // there longer might be better choices for selection.
  ring_concat(p, target);
  *target = (*target)->hd.next;
}

static void try_demote_pool(pool *p)
{
  pool_class pool_class = pool_class_demote(p);
  if (pool_class == NO_CHANGE) return;
  if (p == p->hd.next &&
      (p == pools.young_available || p == pools.old_available)) {
    // Ignore the pool currently used for allocation if it is the last
    // one standing.
    return;
  }
  pool_remove(p);
  if (pool_class == EMPTY) p->hd.class = UNTRACKED;
  pool_reclassify(p, pool_class);
}

static void promote_young_pools()
{
  pool *former_young_pool = pools.young_available;
  ring_concat(pools.young_full, &pools.young_available);
  pools.young_full = NULL;
  while (pools.young_available != NULL) {
    pool *p = ring_pop(&pools.young_available);
    pool_class pool_class = pool_class_promote(p);
    assert(pool_class != NO_CHANGE);
    // A young pool can be empty if it has not been allocated
    // into yet, or if it is the last available young pool.
    p->hd.class = (pool_class == EMPTY) ? UNTRACKED : OLD;
    pool_reclassify(p, pool_class);
  }
  // Now ensure [pools.young_available != NULL]
  pool *new_young_pool = populate_pools(YOUNG);
  if (new_young_pool == NULL) {
    // Memory allocation failed somehow. Since this is called during
    // minor collection, we cannot fail here, so we put back the
    // former head young pool. If this happens again inside a boxroot
    // allocation, fail for real then.
    pool_remove(former_young_pool);
    former_young_pool->hd.class = YOUNG;
    ring_concat(former_young_pool, &pools.young_available);
  }
}

/* }}} */

/* {{{ Allocation, deallocation */

static value * alloc_boxroot_slow(class class);

// hot path
static inline value * alloc_boxroot(class class)
{
  if (DEBUG) assert(class != UNTRACKED);
  pool *p = (class == YOUNG) ? pools.young_available : pools.old_available;
  if (DEBUG) assert(p != NULL);
  slot *new_root = p->hd.free_list;
  if (DEBUG) {
    int a = new_root != &p->roots[POOL_ROOTS_CAPACITY];
    int b = !is_last_elem(new_root);
    int c = p->hd.alloc_count != POOL_ROOTS_CAPACITY;
    assert(a == b && b == c);
  }
  if (LIKELY(!is_last_elem(new_root))) {
    p->hd.free_list = (slot *)*new_root;
    p->hd.alloc_count++;
    return (value *)new_root;
  }
  return alloc_boxroot_slow(class);
}

// Place an available pool in front of the ring and allocate from it.
static value * alloc_boxroot_slow(class class)
{
  // TODO Latency: bound the number of young roots alloced at each
  // minor collection by scheduling a minor collection.
  assert(class != UNTRACKED);
  pool **available_pools = (class == YOUNG) ?
    &pools.young_available : &pools.old_available;
  pool *full = ring_pop(available_pools);
  assert(pool_class_promote(full) == QUASI_FULL);
  assert(class == full->hd.class);
  pool_reclassify(full, QUASI_FULL);
  pool *p = populate_pools(class);
  if (p == NULL) return NULL;
  assert(!is_last_elem(p->hd.free_list) && p->hd.class == class);
  return alloc_boxroot(class);
}

// hot path
static inline void free_boxroot(value *root)
{
  slot *s = (slot *)root;
  pool *p = get_pool_header(s);
  *s = (slot)p->hd.free_list;
  p->hd.free_list = s;
  if (DEBUG) assert(p->hd.alloc_count > 0);
  if (UNLIKELY(is_alloc_threshold(--p->hd.alloc_count))) {
    try_demote_pool(p);
  }
}

/* }}} */

/* {{{ Boxroot API implementation */

// hot path
static inline class classify_value(value v)
{
  if (!Is_block(v)) return UNTRACKED;
  if (is_young(v)) return YOUNG;
  return OLD;
}

// hot path
static inline class classify_boxroot(boxroot root)
{
  return classify_value(*(value *)root);
}

// hot path
static inline boxroot boxroot_create_classified(value init, class class)
{
  if (DEBUG) ++stats.total_create;
  value *cell;
  if (LIKELY(class != UNTRACKED)) {
    cell = alloc_boxroot(class);
  } else {
    cell = (value *) malloc(sizeof(value));
    // TODO: further optim: use a global table instead of malloc for
    // very small values of [init] for fast variants.
  }
  if (LIKELY(cell != NULL)) *cell = init;
  return (boxroot)cell;
}

boxroot boxroot_create(value init)
{
  return boxroot_create_classified(init, classify_value(init));
}

value const * boxroot_get(boxroot root)
{
  return (value *)root;
}

// hot path
static inline void boxroot_delete_classified(boxroot root, class class)
{
  if (DEBUG) ++stats.total_delete;
  value *cell = (value *)root;
  if (LIKELY(class != UNTRACKED)) {
    free_boxroot(cell);
  } else {
    free(cell);
  }
}

// hot path
void boxroot_delete(boxroot root)
{
  CAMLassert(root);
  boxroot_delete_classified(root, classify_boxroot(root));
}

// hot path
void boxroot_modify(boxroot *root_ref, value new_value)
{
  boxroot root = *root_ref;
  CAMLassert(root);
  if (DEBUG) ++stats.total_modify;
  class old_class = classify_boxroot(root);
  class new_class = classify_value(new_value);

  if (old_class == new_class
      || (old_class == YOUNG && new_class == OLD)) {
    // No need to reallocate
    *(value *)root = new_value;
    return;
  }

  boxroot_delete_classified(root, old_class);
  *root_ref = boxroot_create_classified(new_value, new_class);
  // Note: *root_ref can become NULL, which must be checked explicitly
  // (in Rust, check and panic here).
}

/* }}} */

/* {{{ Scanning */

static void validate_pool(pool *pool)
{
  if (pool->hd.free_list == NULL) {
    // an unintialised pool
    assert(pool->hd.class == UNTRACKED);
    return;
  }
  slot *pool_end = &pool->roots[POOL_ROOTS_CAPACITY];
  // check freelist structure and length
  slot *curr = pool->hd.free_list;
  int length = 0;
  while (curr != pool_end) {
    length++;
    assert(length <= POOL_ROOTS_CAPACITY);
    assert(curr >= pool->roots && curr < pool_end);
    slot s = *curr;
    curr = (slot *)s;
  }
  assert(length == POOL_ROOTS_CAPACITY - pool->hd.alloc_count);
  // check count of allocated elements
  int alloc_count = 0;
  for(int i = 0; i < POOL_ROOTS_CAPACITY; i++) {
    slot v = pool->roots[i];
    if (!is_pool_member_nostats(v, pool)) {
      if (pool->hd.class != YOUNG && !Is_block((value)v))
        assert(!Is_young((value)v));
      ++alloc_count;
    }
  }
  assert(alloc_count == pool->hd.alloc_count);
}

static void validate_all_pools()
{
  assert(pools.young_available != NULL);
  assert(pools.old_available != NULL);
  FOREACH_GLOBAL_RING(global, class, {
      pool *start_pool = *global;
      if (start_pool == NULL) continue;
      pool *p = start_pool;
      do {
        assert(p->hd.class == class);
        validate_pool(p);
        assert(p->hd.next != NULL);
        assert(p->hd.next->hd.prev == p);
        assert(p->hd.prev != NULL);
        assert(p->hd.prev->hd.next == p);
        p = p->hd.next;
      } while (p != start_pool);
    });
}

// sort pool in increasing sequence
static void defrag_pool(pool * pool)
{
  slot *current = pool->roots;
  slot *pool_end = &pool->roots[POOL_ROOTS_CAPACITY];
  slot **freelist_last = &pool->hd.free_list;
  for (; current != pool_end; ++current) {
    slot v = *current;
    if (!is_pool_member(v, pool)) continue;
    *freelist_last = current;
    freelist_last = (slot **)current;
    ++stats.defrag_sort;
  }
  *freelist_last = pool_end;
}

#define MINOR_SCANNING_ACTION caml_oldify_one
#define MAJOR_SCANNING_ACTION caml_darken
#define COMPACT_SCANNING_ACTION caml_invert_root

// returns the amount of work done
static inline int scan_pool(scanning_action action, pool *pool, int do_old)
{
  int allocs_to_find = pool->hd.alloc_count;
  slot *current = pool->roots;
  while (allocs_to_find) {
    // hot path
    slot s = *current;
    if (LIKELY((!is_pool_member(s, pool)))) {
      --allocs_to_find;
      value v = (value)s;
      if (DEBUG && Is_young(v)) ++stats.young_hit;
      action(v, (value *)current);
    }
    ++current;
  }
  return current - pool->roots;
}

// returns the amount of work done
static int scan_pool_dispatch(scanning_action action, pool * pool)
{
  int work = 0;
  if (action == &COMPACT_SCANNING_ACTION) {
    defrag_pool(pool);
    work += POOL_ROOTS_CAPACITY;
  }
  // do a static dispatch whenever useful
  if (action == &MINOR_SCANNING_ACTION) {
    work += scan_pool(MINOR_SCANNING_ACTION, pool, 0);
  } else if (action == &MAJOR_SCANNING_ACTION) {
    work += scan_pool(MAJOR_SCANNING_ACTION, pool, 1);
  } else {
    work += scan_pool(action, pool, 1);
  }
  return work;
}

static int scan_pools(scanning_action action, int for_minor)
{
  int work = 0;
  FOREACH_GLOBAL_RING(global, class, {
      if (class == UNTRACKED || (for_minor && class == OLD)) continue;
      pool *start_pool = *global;
      if (start_pool == NULL) continue;
      pool *p = start_pool;
      do {
        work += scan_pool_dispatch(action, p);
        p = p->hd.next;
      } while (p != start_pool);
    });
  return work;
}

static void scan_roots(scanning_action action)
{
  if (DEBUG) validate_all_pools();
  int for_minor = (action == &MINOR_SCANNING_ACTION);
  int work = scan_pools(action, for_minor);
  if (for_minor) {
    promote_young_pools();
    stats.total_scanning_work_minor += work;
  } else {
    stats.total_scanning_work_major += work;
  }
  if (action == &COMPACT_SCANNING_ACTION) {
    stats.total_freed_pools += free_all_chunks(pools.free);
    pools.free = NULL;
  }
  if (DEBUG) validate_all_pools();
}

/* }}} */

/* {{{ Statistics */

// 1=KiB, 2=MiB
static int kib_of_pools(int count, int unit)
{
  int log_per_pool = POOL_LOG_SIZE - unit * 10;
  if (log_per_pool >= 0) return count << log_per_pool;
  /* log_per_pool < 0) */
  return count >> -log_per_pool;
}

static int average(long long total_work, int nb_collections) {
    if (nb_collections <= 0)
        return -1;
    // round to nearest
    return (total_work + (nb_collections / 2)) / nb_collections;
}

static int boxroot_used()
{
  FOREACH_GLOBAL_RING (global, class, {
      if (class == UNTRACKED) continue;
      pool *p = *global;
      if (p != NULL && (p->hd.alloc_count != 0 || p->hd.next != p)) {
        return 1;
      }
    });
  return 0;
}

void boxroot_print_stats()
{
#if PRINT_STATS != 0
  setlocale(LC_ALL, "en_US.UTF-8");
#endif

  printf("minor collections: %d\n"
         "major collections: %d\n",
         stats.minor_collections,
         stats.major_collections);

  int scanning_work_minor = average(stats.total_scanning_work_minor, stats.minor_collections);
  int scanning_work_major = average(stats.total_scanning_work_major, stats.major_collections);
  long long total_scanning_work = stats.total_scanning_work_minor + stats.total_scanning_work_major;
  int ring_operations_per_pool = average(stats.ring_operations, stats.total_alloced_pools);
  int total_mib = kib_of_pools(stats.total_alloced_pools, 2);
  int freed_mib = kib_of_pools(stats.total_freed_pools, 2);
  int peak_mib = kib_of_pools(stats.peak_pools, 2);

  if (!boxroot_used() && total_scanning_work == 0) return;

  printf("POOL_LOG_SIZE: %d (%'d KiB, %'d roots)\n"
         "USE_MMAP: %d\n"
         "USE_MADV_HUGEPAGE: %d\n"
         "USE_SUPERBLOCK: %d\n"
         "SUPERBLOCK_LOG_SIZE: %d\n"
         "DEFRAG: %d\n"
         "DEBUG: %d\n"
         "WITH_EXPECT: %d\n",
         (int)POOL_LOG_SIZE, kib_of_pools((int)1, 1), (int)POOL_ROOTS_CAPACITY,
         (int)USE_MMAP,
         (int)USE_MADV_HUGEPAGE,
         (int)USE_SUPERBLOCK,
         (int)SUPERBLOCK_LOG_SIZE,
         (int)DEFRAG,
         (int)DEBUG,
         (int)WITH_EXPECT);

  printf("CHUNK_SIZE: %'d kiB (%'d pools)\n"
         "CHUNK_ALIGNMENT: %'d kiB\n"
         "total allocated chunks: %'d (%'d pools)\n",
         kib_of_pools(POOLS_PER_CHUNK,1), (int)POOLS_PER_CHUNK,
         kib_of_pools(CHUNK_ALIGNMENT / POOL_SIZE,1),
         stats.total_alloced_chunks, stats.total_alloced_chunks * (int)POOLS_PER_CHUNK);

  printf("total allocated pools: %'d (%'d MiB)\n"
         "total freed pools: %'d (%'d MiB)\n"
         "peak allocated pools: %'d (%'d MiB)\n",
         stats.total_alloced_pools, total_mib,
         stats.total_freed_pools, freed_mib,
         stats.peak_pools, peak_mib);

  printf("work per minor: %'d\n"
         "work per major: %'d\n"
         "total scanning work: %'lld (%'lld minor, %'lld major)\n",
         scanning_work_minor,
         scanning_work_major,
         total_scanning_work, stats.total_scanning_work_minor, stats.total_scanning_work_major);

  printf("total ring operations: %'d\n"
         "ring operations per pool: %'d\n",
         stats.ring_operations,
         ring_operations_per_pool);

  printf("defrag (sort): %'d\n",
         stats.defrag_sort);

#if DEBUG != 0
  printf("total created: %'d\n"
         "total deleted: %'d\n"
         "total modified: %'d\n",
         stats.total_create,
         stats.total_delete,
         stats.total_modify);

  printf("is_young: %'lld\n"
         "young_hit: %d%%\n"
         "get_pool_header: %'lld\n"
         "is_pool_member: %'lld\n"
         "is_last_elem: %'lld\n",
         stats.is_young,
         (int)((stats.young_hit * 100) / stats.total_scanning_work_minor),
         stats.get_pool_header,
         stats.is_pool_member,
         stats.is_last_elem);
#endif
}

/* }}} */

/* {{{ Hook setup */

static void (*boxroot_prev_scan_roots_hook)(scanning_action);

static void scanning_callback(scanning_action action)
{
  if (boxroot_prev_scan_roots_hook != NULL) {
    (*boxroot_prev_scan_roots_hook)(action);
  }
  if (action == &MINOR_SCANNING_ACTION) {
    ++stats.minor_collections;
  } else {
    ++stats.major_collections;
  }
  // If no boxroot has been allocated, then scan_roots should not have
  // any noticeable cost. For experimental purposes, since this hook
  // is also used for other the statistics of other implementations,
  // we further make sure of this with an extra test, by avoiding
  // calling scan_roots if it has only just been initialised.
  if (boxroot_used()) scan_roots(action);
}

static int setup = 0;

// Must be called to set the hook
int boxroot_setup()
{
  if (setup) return 0;
  boxroot_prev_scan_roots_hook = caml_scan_roots_hook;
  caml_scan_roots_hook = scanning_callback;
  FOREACH_GLOBAL_RING(global, cl, { *global = NULL; });
  if (!populate_pools(YOUNG) || !populate_pools(OLD)) goto fail;
  setup = 1;
  return 1;
fail:
  caml_scan_roots_hook = boxroot_prev_scan_roots_hook;
  return 0;
}

value boxroot_scan_hook_setup(value unit)
{
  if (!boxroot_setup()) caml_failwith("boxroot_scan_hook_setup");
  return unit;
}

void boxroot_teardown()
{
  if (!setup) return;
  caml_scan_roots_hook = boxroot_prev_scan_roots_hook;
  FOREACH_GLOBAL_RING(global, cl, { free_all_chunks(*global); });
  setup = 0;
}

value boxroot_scan_hook_teardown(value unit)
{
  if (PRINT_STATS) boxroot_print_stats();
  boxroot_teardown();
  return unit;
}

/* }}} */

/* {{{ */
/* }}} */
