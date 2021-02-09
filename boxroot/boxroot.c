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

// Options

#define POOL_LOG_SIZE 12 // 12 = 4KB
/* Print statistics on teardown? */
#define PRINT_STATS 1
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
/* Defragment during scanning? (disabled for now)
   + Better cache locality for successive allocations after lots of
     deallocations.
   + Improves early exit during scanning after lots of deallocations.
   - Scanning is a lot more expensive (read-write instead of read-only).
   TODO:
    - Implement defrag on compaction
    - Implement defrag on demotion
*/
#define DEFRAG 0
/* Check integrity of pool structure after each scan? (slow) */
#define DEBUG 0
/* Use __builtin_expect in hot paths? (suggested by suggested by
   looking at the generated assembly in godbolt)
   0 = off
   1 = on
   2 = invert
 */
#define WITH_EXPECT 1
/* Trades a few occasional segfaults for a faster test of membership
   to the minor heap. */
#define FAST_IS_YOUNG 0

// end of options

// Setup

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

// Data types

typedef void * slot;

struct header {
  struct pool *prev;
  struct pool *next;
  slot *free_list;
  int alloc_count;
};

#define POOL_ROOTS_CAPACITY                               \
  ((POOL_SIZE - sizeof(struct header)) / sizeof(slot) - 1)
/* &pool->roots[POOL_ROOTS_CAPACITY] can end up as a placeholder value
   in the freelist to denote the last element of the freelist,
   starting from after releasing from a full pool for the first
   time. To ensure that this value is recognised by the test
   [is_pool_member(v, pool)], we subtract one from the capacity. */

typedef struct pool {
  struct header hd;
  /* Unoccupied slots are either NULL or a pointer to the next free
     slot. The null value acts as a terminator: if a slot is null,
     then all subsequent slots are null (bump pointer optimisation). */
  slot roots[POOL_ROOTS_CAPACITY];
  uintptr_t end;// unused
} pool;

static_assert(POOL_ROOTS_CAPACITY <= INT_MAX, "capacity too large");
static_assert(sizeof(pool) == POOL_SIZE, "bad pool size");

// hot path
static inline pool * get_pool_header(slot v)
{
  return (pool *)((uintptr_t)v & ~((uintptr_t)POOL_SIZE - 1));
}

// hot path
static inline int is_pool_member(slot v, pool *p)
{
  return get_pool_header(v) == p;
}

#define POOL_BITMASK ((uintptr_t)(POOL_SIZE - sizeof(slot)))

/*
static void printbinary(uintptr_t n)
{
  printf("\n");
  for (int i = 63; i >= 0; i--) {
    if (n & ((uintptr_t)1 << i))
      printf("1");
    else
      printf("0");
  }
  printf("\n");
}*/

#define YOUNG_MASK (~(((uintptr_t)1 << 22) - 1))

static uintptr_t young_mask_value = 0;

// hot path
static inline int is_young(value v)
{
#if FAST_IS_YOUNG
  return ((uintptr_t)v & YOUNG_MASK) == young_mask_value;
#else
  return Is_young(v);
#endif
}

static void init_young_mask()
{
  young_mask_value = (uintptr_t)caml_young_start & YOUNG_MASK;
  assert(is_young((value)(caml_young_start + 1)));
  assert(is_young((value)(caml_young_end - 1)));
}

// hot path
static inline int is_last_elem(slot *v)
{
  return ((uintptr_t)v & POOL_BITMASK) == POOL_BITMASK;
}

// Rings of pools
static pool *old_pools = NULL; // Contains only roots pointing to the
                               // major heap. Never NULL.
static pool *young_pools = NULL; // Contains roots pointing to the
                                 // major or the minor heap. Never
                                 // NULL.
static pool *free_pools = NULL; // Contains free uninitialized pools

typedef enum class {
  YOUNG,
  OLD,
  UNTRACKED
} class;

static struct {
  int minor_collections;
  int major_collections;
  int total_create;
  int total_delete;
  int total_modify;
  int total_scanning_work_minor;
  int total_scanning_work_major;
  int total_alloced_chunks;
  int total_alloced_pools;
  int total_freed_pools;
  int live_pools;
  int peak_pools;
  int get_available_pool_seeking;
  int ring_operations; // Number of times hd.next is mutated
  int defrag_sort;
} stats; // zero-initialized


// Platform-specific

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
  if (PRINT_STATS) ++stats.total_alloced_chunks;
  return p;
}

static void free_chunk(void *p)
{
  if (USE_MMAP) {
    // TODO: not implemented
    //munmap(p, POOL_SIZE);
  } else {
    free(p);
  }
}

// Ring operations

static void ring_init(pool *p)
{
  p->hd.next = p;
  p->hd.prev = p;
  if (PRINT_STATS) ++stats.ring_operations;
}

// insert the ring [source] in front of [*target]
static void ring_concat(pool *source, pool **target)
{
  if (source == NULL) return;
  pool *old = *target;
  if (old == NULL) {
    *target = source;
  } else {
    pool *last = old->hd.prev;
    last->hd.next = source;
    source->hd.prev->hd.next = old;
    old->hd.prev = source->hd.prev;
    source->hd.prev = last;
    *target = source;
    if (PRINT_STATS) stats.ring_operations += 2;
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
  if (PRINT_STATS) ++stats.ring_operations;
  *target = front->hd.next;
  ring_init(front);
  return front;
}

// Pool management

static pool * get_free_pool()
{
  if (free_pools != NULL) {
    return ring_pop(&free_pools);
  }
  pool *chunk = alloc_chunk();
  if (chunk == NULL) return NULL;
  pool *p;
  for (p = chunk + POOLS_PER_CHUNK - 1; p >= chunk; p--) {
    ring_init(p);
    ring_concat(p, &free_pools);
  }
  return ring_pop(&free_pools);
}

static pool * alloc_pool()
{
  if (PRINT_STATS) {
    ++stats.total_alloced_pools;
    ++stats.live_pools;
    if (stats.live_pools > stats.peak_pools)
      stats.peak_pools = stats.live_pools;
  }
  pool *out = get_free_pool();

  if (out == NULL) return NULL;

  out->hd.free_list = out->roots;
  out->hd.alloc_count = 0;
  slot *end = &out->roots[POOL_ROOTS_CAPACITY];
  slot *s = out->roots;
  while (s < end) {
    slot *next = s + 1;
    *s = next;
    s = next;
  }

  return out;
}

// Find an available pool for the class; place it in front of the ring
// and return it. Return NULL if none was found and the allocation of
// a new one failed.
static pool * get_available_pool(class class)
{
  assert(class != UNTRACKED);
  pool **pool_ring = (class == YOUNG) ? &young_pools : &old_pools;
  pool *start_pool = *pool_ring;

  if (start_pool != NULL) {
    assert(start_pool->hd.alloc_count == POOL_ROOTS_CAPACITY);

    // Find a pool with available slots. TODO: maybe faster lookup by
    // putting the more empty pools in the front during scanning, and/or
    // temporarily place full ones in an auxiliary ring (until next
    // scan).
    for (pool *next_pool = start_pool->hd.next;
         next_pool != start_pool;
         next_pool = next_pool->hd.next) {
      if (PRINT_STATS) ++stats.get_available_pool_seeking;
      if (next_pool->hd.alloc_count < POOL_ROOTS_CAPACITY) {
        // Rotate the ring, making the pool with free slots the head
        *pool_ring = next_pool;
        return next_pool;
      }
    }
  }

  // None found, add a new pool at the start
  pool *new_pool = alloc_pool();
  if (new_pool == NULL) return NULL;
  ring_concat(new_pool, pool_ring);

  return new_pool;
}

// Free a pool if empty and not the last of its ring.
static void try_free_pool(pool *p)
{
  if (p->hd.alloc_count != 0 || p->hd.next == p) return;

  pool *hd = ring_pop(&p);
  if (old_pools == hd) old_pools = p;
  else if (young_pools == hd) young_pools = p;
  assert(hd != free_pools);
  if (USE_SUPERBLOCK) {
    // TODO: implement reclamation
    ring_concat(hd, &free_pools);
  } else {
    free_chunk(hd);
  }
  if (PRINT_STATS) stats.total_freed_pools++;
}

// Allocation, deallocation

static value * alloc_boxroot_slow(class class);

// hot path
static inline value * alloc_boxroot(class class)
{
  if (DEBUG) assert(class != UNTRACKED);
  pool *p = (class == YOUNG) ? young_pools : old_pools;
  slot * new_root = p->hd.free_list;
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
  pool *p = get_available_pool(class);
  if (p == NULL) return NULL;
  assert(!is_last_elem(p->hd.free_list));
  return alloc_boxroot(class);
}

// hot path
static inline void free_boxroot(value *root)
{
  pool *p = get_pool_header((slot)root);
  *(slot *)root = p->hd.free_list;
  p->hd.free_list = (slot)root;
  if (UNLIKELY(--p->hd.alloc_count == 0)) {
    try_free_pool(p);
  }
}

// Scanning

static int validate_pool(pool *pool, int in_young)
{
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
    if (!is_pool_member(v, pool)) {
      if (!in_young) assert(!is_young((value)v));
      ++alloc_count;
    }
  }
  assert(alloc_count == pool->hd.alloc_count);
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
    if (PRINT_STATS) ++stats.defrag_sort;
  }
  *freelist_last = pool_end;
}

#define MINOR_SCANNING_ACTION caml_oldify_one
#define MAJOR_SCANNING_ACTION caml_darken
#define COMPACT_SCANNING_ACTION caml_invert_root

#define SCAN_POOL(action, pool) do {                        \
    int allocs_to_find = pool->hd.alloc_count;              \
    slot *current = pool->roots;                            \
    while (allocs_to_find) {                                \
      slot v = *current;                                    \
      if ((!is_pool_member(v, pool))) {                     \
        action((value)v, (value *) current);                \
        --allocs_to_find;                                   \
      }                                                     \
      ++current;                                            \
    }                                                       \
    work += current - pool->roots;                          \
  } while (0)

static inline int is_young_member(slot v, pool *p)
{
  return is_young((value)v);
}

// returns the amount of work done
static int scan_pool(scanning_action action, pool * pool)
{
  int work = 0;
  if (action == &COMPACT_SCANNING_ACTION) {
    defrag_pool(pool);
    work += POOL_ROOTS_CAPACITY;
  }
  if (action == &MINOR_SCANNING_ACTION) {
    SCAN_POOL(MINOR_SCANNING_ACTION, pool);
  } else if (action == &MAJOR_SCANNING_ACTION) {
    SCAN_POOL(MAJOR_SCANNING_ACTION, pool);
  } else {
    // reference implementation
    SCAN_POOL(action, pool);
  }
  return work;
}

static int scan_pools(scanning_action action, pool *start_pool)
{
  int work = 0;
  if (start_pool == NULL) return work;
  work += scan_pool(action, start_pool);
  if (DEBUG) validate_pool(start_pool, start_pool == young_pools);
  for (pool *pool = start_pool->hd.next;
       pool != start_pool;
       pool = pool->hd.next) {
    work += scan_pool(action, pool);
    if (DEBUG) validate_pool(pool, start_pool == young_pools);
  }
  return work;
}

static void scan_for_minor(scanning_action action)
{
  ++stats.minor_collections;
  if (young_pools == NULL) return;
  int work = scan_pools(action, young_pools);
  stats.total_scanning_work_minor += work;
  // promote minor pools
  pool *new_young_pool = ring_pop(&young_pools);
  ring_concat(young_pools, &old_pools);
  young_pools = new_young_pool;
}

static void scan_all(scanning_action action)
{
  ++stats.major_collections;
  int work = scan_pools(action, young_pools);
  work += scan_pools(action, old_pools);
  stats.total_scanning_work_major += work;
}

static void (*boxroot_prev_scan_roots_hook)(scanning_action);

static void boxroot_scan_roots(scanning_action action)
{
  if (action == &MINOR_SCANNING_ACTION) {
    scan_for_minor(action);
  } else {
    scan_all(action);
  }
  if (boxroot_prev_scan_roots_hook != NULL) {
    (*boxroot_prev_scan_roots_hook)(action);
  }
}

// 1=KiB, 2=MiB
static int kib_of_pools(int count, int unit)
{
  int log_per_pool = POOL_LOG_SIZE - unit * 10;
  if (log_per_pool >= 0) return count << log_per_pool;
  if (log_per_pool < 0) return count >> -log_per_pool;
}

static int average(int total_work, int nb_collections) {
    if (nb_collections <= 0)
        return -1;
    // round to nearest
    return ((total_work + (nb_collections / 2)) / nb_collections);
}

static void print_stats()
{
  printf("minor collections: %d\n"
         "major collections: %d\n",
         stats.minor_collections,
         stats.major_collections);

  int scanning_work_minor = average(stats.total_scanning_work_minor, stats.minor_collections);
  int scanning_work_major = average(stats.total_scanning_work_major, stats.major_collections);
  int total_scanning_work = stats.total_scanning_work_minor + stats.total_scanning_work_major;
  int ring_operations_per_pool = average(stats.ring_operations, stats.total_alloced_pools);
  int total_mib = kib_of_pools(stats.total_alloced_pools, 2);
  int freed_mib = kib_of_pools(stats.total_freed_pools, 2);
  int peak_mib = kib_of_pools(stats.peak_pools, 2);

  if (total_scanning_work == 0 && stats.total_alloced_pools <= 2)
    return;

  printf("POOL_LOG_SIZE: %d (%d KiB, %d roots)\n"
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

  printf("CHUNK_SIZE: %d kiB (%d pools)\n"
         "CHUNK_ALIGNMENT: %d kiB\n"
         "total allocated chunks: %d (%d pools)\n",
         kib_of_pools(POOLS_PER_CHUNK,1), (int)POOLS_PER_CHUNK,
         kib_of_pools(CHUNK_ALIGNMENT / POOL_SIZE,1),
         stats.total_alloced_chunks, stats.total_alloced_chunks * (int)POOLS_PER_CHUNK);

  printf("total allocated pools: %d (%d MiB)\n"
         "total freed pools: %d (%d MiB)\n"
         "peak allocated pools: %d (%d MiB)\n"
         "get_available_pool seeking: %d\n",
         stats.total_alloced_pools, total_mib,
         stats.total_freed_pools, freed_mib,
         stats.peak_pools, peak_mib,
         stats.get_available_pool_seeking);

  printf("work per minor: %d\n"
         "work per major: %d\n"
         "total scanning work: %d\n",
         scanning_work_minor,
         scanning_work_major,
         total_scanning_work);

  printf("total created: %d\n"
         "total deleted: %d\n"
         "total modified: %d\n",
         stats.total_create,
         stats.total_delete,
         stats.total_modify);

  printf("total ring operations: %d\n"
         "ring operations per pool: %d\n",
         stats.ring_operations,
         ring_operations_per_pool);

  printf("defrag (sort): %d\n",
         stats.defrag_sort);
}

// Must be called to set the hook
value boxroot_scan_hook_setup(value unit)
{
  boxroot_prev_scan_roots_hook = caml_scan_roots_hook;
  caml_scan_roots_hook = boxroot_scan_roots;
  young_pools = alloc_pool();
  old_pools = alloc_pool();
  init_young_mask();
  return Val_unit;
}

static void force_free_pools(pool *start)
{
  if (USE_SUPERBLOCK) {
    // TODO: not implemented
    return;
  }
  if (start == NULL) return;
  pool *p = start;
  do {
    pool *next = p->hd.next;
    free_chunk(p);
    p = next;
  } while (p != start);
}

value boxroot_scan_hook_teardown(value unit)
{
  caml_scan_roots_hook = boxroot_prev_scan_roots_hook;
  boxroot_prev_scan_roots_hook = NULL;
  if (PRINT_STATS) print_stats();
  force_free_pools(young_pools);
  force_free_pools(old_pools);
  return Val_unit;
}

// Boxroot API implementation

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
  if (PRINT_STATS) ++stats.total_create;
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
  if (PRINT_STATS) ++stats.total_delete;
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
  assert(root);
  boxroot_delete_classified(root, classify_boxroot(root));
}

// hot path
void boxroot_modify(boxroot *root_ref, value new_value)
{
  boxroot root = *root_ref;
  assert(root);
  if (PRINT_STATS) ++stats.total_modify;
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
  // Note: *root_ref can become NULL, which must be checked (in Rust,
  // check and panic here).
}
