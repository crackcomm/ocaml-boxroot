#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAML_INTERNALS

#include "boxroot.h"
#include <caml/roots.h>
#include <caml/minor_gc.h>

// Options

#define POOL_LOG_SIZE 12 // 4KB
#define LOW_CAPACITY_THRESHOLD 50 /* 50% capacity before promoting a
                                     young pool. */
/* Print statistics on teardown? */
#define PRINT_STATS 1
/* Allocate with mmap?
   USE_MMAP requires POOL_SIZE to be equal the OS page size for now. */
#define USE_MMAP 0

// Data types

#define POOL_SIZE (1 << POOL_LOG_SIZE)

typedef void * slot;

struct header {
  struct pool *prev;
  struct pool *next;
  slot *free_list;
  size_t free_count;
  size_t capacity; /* Number of non-null slots, updated at the end of
                      each scan. */
};

#define POOL_ROOTS_CAPACITY ((POOL_SIZE - sizeof(struct header)) / sizeof(slot))

typedef struct pool {
  struct header hd;
  /* Unoccupied slots are either NULL or a pointer to the next free
     slot. The null value acts as a terminator: if a slot is null,
     then all subsequent slots are null (bump pointer optimisation). */
  slot roots[POOL_ROOTS_CAPACITY];
} pool;

static_assert(sizeof(pool) == POOL_SIZE, "bad pool size");

// hot path
static inline pool * get_pool_header(slot v)
{
  return (pool *)((uintptr_t)v & ~((uintptr_t)POOL_SIZE - 1));
}

// Rings of pools
static pool *old_pools = NULL; // Contains only roots pointing to
                                 // the major heap
static pool *young_pools = NULL; // Contains roots pointing to the
                                   // major or the minor heap

typedef enum class {
  YOUNG,
  OLD,
  UNTRACKED
} class;

static struct {
  int minor_collections;
  int major_collections;
  int total_scanning_work_minor;
  int total_scanning_work_major;
  int total_alloced_pools;
  int total_freed_pools;
  int live_pools;
  int peak_pools;
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
  if (USE_MMAP) {
    if (POOL_SIZE != sysconf(_SC_PAGESIZE)) return NULL;
    void *mem = mmap(0, POOL_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (mem == MAP_FAILED) ? NULL : mem;
  } else {
    return aligned_alloc(POOL_SIZE, POOL_SIZE); // TODO: not portable
  }
}

static void free_chunk(void *p)
{
  if (USE_MMAP) {
    munmap(p, POOL_SIZE);
  } else {
    free(p);
  }
}

// Pool management

static pool * alloc_pool()
{
  ++stats.total_alloced_pools;
  ++stats.live_pools;
  if (stats.live_pools > stats.peak_pools) stats.peak_pools = stats.live_pools;

  pool *out = alloc_chunk();

  if (out == NULL) return NULL;

  out->hd.prev = out->hd.next = out;
  out->hd.free_list = out->roots;
  out->hd.free_count = POOL_ROOTS_CAPACITY;
  out->hd.capacity = 0;
  memset(out->roots, 0, sizeof(out->roots));

  return out;
}

// insert [source] in front of [*target]
static void ring_insert(pool *source, pool **target)
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
    *target = young_pools;
  }
}

// remove the first element from [*target] and return it
static pool * ring_pop(pool **target)
{
  pool *front = *target;
  if (front->hd.next == front) {
    *target = NULL;
    return front;
  }
  front->hd.prev->hd.next = front->hd.next;
  front->hd.next->hd.prev = front->hd.prev;
  *target = front->hd.next;
  front->hd.next = front;
  front->hd.prev = front;
  return front;
}

// Find an available pool for the class; place it in front of the ring
// and return it. Return NULL if none was found and the allocation of
// a new one failed.
static pool * get_available_pool(class class)
{
  // If there was no optimisation for immediates, we could always place
  // the immediates with the old values (be careful about NULL in
  // naked-pointers mode, though).
  pool **pool_ring = (class == YOUNG) ? &young_pools : &old_pools;
  pool *start_pool = *pool_ring;

  if (start_pool != NULL) {
    CAMLassert(start_pool->hd.free_count == 0);

    // Find a pool with available slots. TODO: maybe faster lookup by
    // putting the more empty pools in the front during scanning, and/or
    // temporarily place full ones in an auxiliary ring (until next
    // scan).
    for (pool *next_pool = start_pool->hd.next;
         next_pool != start_pool;
         next_pool = next_pool->hd.next) {
      if (next_pool->hd.free_count > 0) {
        // Rotate the ring, making the pool with free slots the head
        *pool_ring = next_pool;
        return next_pool;
      }
    }
  }

  // None found, add a new pool at the start
  pool *new_pool = alloc_pool();
  if (new_pool == NULL) return NULL;
  ring_insert(new_pool, pool_ring);

  return new_pool;
}

// Free a pool if empty and not the last of its ring.
static void try_free_pool(pool *p)
{
  if (p->hd.free_count != POOL_ROOTS_CAPACITY || p->hd.next == p) return;

  pool *hd = ring_pop(&p);
  if (old_pools == hd) old_pools = p;
  else if (young_pools == hd) young_pools = p;
  free_chunk(hd);
  stats.total_freed_pools++;
}

// Allocation, deallocation

// hot path
static inline value * freelist_pop(pool *p)
{
  slot *new_root = p->hd.free_list;
  slot next = *new_root;
  p->hd.free_count--;

  // [new_root] contains either a pointer to the next free slot or
  // NULL. If it is NULL, the next free slot is adjacent to the
  // current one.
  p->hd.free_list = (next == NULL) ? new_root+1 : (slot *)next;
  return (value *)new_root;
}

// slow path: Place an available pool in front of the ring and
// allocate from it.
static value * alloc_boxroot_slow(class class)
{
  CAMLassert(class != UNTRACKED);
  pool *p = get_available_pool(class);
  if (p == NULL) return NULL;
  CAMLassert(pool_is_available(p));
  return freelist_pop(p);
}

// hot path
static inline value * alloc_boxroot(class class)
{
  // TODO Latency: bound the number of young roots alloced at each
  // minor collection by scheduling a minor collection.
  pool *p = (class == YOUNG) ? young_pools : old_pools;
  // Test for NULL is necessary here: it is not always possible to
  // allocate the first pool elsewhere, e.g. in scanning functions
  // which must not fail.
  if (p != NULL && p->hd.free_count != 0) {
    return freelist_pop(p);
  }
  return alloc_boxroot_slow(class);
}

// hot path
static inline void free_boxroot(value *root)
{
  pool *p = get_pool_header((slot)root);
  *(slot *)root = p->hd.free_list;
  p->hd.free_list = (slot)root;
  if (++p->hd.free_count == POOL_ROOTS_CAPACITY) {
    try_free_pool(p);
  }
}

// Scanning

static void (*boxroot_prev_scan_roots_hook)(scanning_action);

static int is_minor_scanning(scanning_action action)
{
  return action == &caml_oldify_one;
}

static int scan_pool(scanning_action action, pool * pool)
{
  int i = 0;
  for (; i < POOL_ROOTS_CAPACITY; ++i) {
    slot v = pool->roots[i];
    if (v == NULL) {
      // We can skip the rest if the pointer value is NULL.
      break;
    }
    if (get_pool_header(v) != pool) {
      // The value is an OCaml block (or possibly an immediate whose
      // msbs differ from those of [pool], if the immediates
      // optimisation were to be turned off).
      (*action)((value)v, (value *) &pool->roots[i]);
    }
  }
  pool->hd.capacity = i;
  return i;
}

static int scan_pools(scanning_action action, pool *start_pool)
{
  int work = 0;
  if (start_pool == NULL) return work;
  work += scan_pool(action, start_pool);
  for (pool *pool = start_pool->hd.next;
       pool != start_pool;
       pool = pool->hd.next) {
    work += scan_pool(action, pool);
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
  pool *new_young_pool = NULL;
  if ((young_pools->hd.capacity * 100 / POOL_ROOTS_CAPACITY) <=
      LOW_CAPACITY_THRESHOLD) {
    new_young_pool = ring_pop(&young_pools);
  }
  ring_insert(young_pools, &old_pools);
  young_pools = new_young_pool;
}

static void scan_for_major(scanning_action action)
{
  ++stats.major_collections;
  int work = scan_pools(action, young_pools);
  work += scan_pools(action, old_pools);
  stats.total_scanning_work_major += work;
}

static void boxroot_scan_roots(scanning_action action)
{
  if (is_minor_scanning(action)) {
    scan_for_minor(action);
  } else {
    scan_for_major(action);
  }
  if (boxroot_prev_scan_roots_hook != NULL) {
    (*boxroot_prev_scan_roots_hook)(action);
  }
}

static int mib_of_pools(int count)
{
  int log_per_pool = POOL_LOG_SIZE - 20;
  if (log_per_pool >= 0) return count << log_per_pool;
  if (log_per_pool < 0) return count >> -log_per_pool;
}

static int average(int total_work, int nb_collections) {
    if (nb_collections <= 0)
        return -1;
    return (total_work / nb_collections);
}

static void print_stats()
{
  printf("minor collections: %d\n"
         "major collections: %d\n",
         stats.minor_collections,
         stats.major_collections);

  int scanning_work_minor = average(stats.total_scanning_work_minor, stats.minor_collections);
  int scanning_work_major = average(stats.total_scanning_work_major, stats.major_collections);
  int total_mib = mib_of_pools(stats.total_alloced_pools);
  int freed_mib = mib_of_pools(stats.total_freed_pools);
  int peak_mib = mib_of_pools(stats.peak_pools);

  if (scanning_work_minor == 0
      && scanning_work_major == 0
      && stats.total_alloced_pools == 0)
    return;

  printf("work per minor: %d\n"
         "work per major: %d\n"
         "total allocated pools: %d (%d MiB)\n"
         "total freed pools: %d (%d MiB)\n"
         "peak allocated pools: %d (%d MiB)\n",
         scanning_work_minor,
         scanning_work_major,
         stats.total_alloced_pools, total_mib,
         stats.total_freed_pools, freed_mib,
         stats.peak_pools, peak_mib);
}

// Must be called to set the hook
value boxroot_scan_hook_setup(value unit)
{
  boxroot_prev_scan_roots_hook = caml_scan_roots_hook;
  caml_scan_roots_hook = boxroot_scan_roots;
  return Val_unit;
}

static void force_free_pools(pool *start)
{
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
  if(v == 0 || !Is_block(v)) return UNTRACKED;
  if(Is_young(v)) return YOUNG;
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
  value *cell;
  if (class != UNTRACKED) {
    cell = alloc_boxroot(class);
  } else {
    // [init] can be null in naked-pointers mode, handled here.
    cell = (value *) malloc(sizeof(value));
    // TODO: further optim: use a global table instead of malloc for
    // very small values of [init] — for fast variants and and to
    // handle C-side NULLs in no-naked-pointers mode if desired.
  }
  if (cell != NULL) *cell = init;
  return (boxroot)cell;
}

boxroot boxroot_create(value init)
{
  return boxroot_create_classified(init, classify_value(init));
}

value const * boxroot_get(boxroot root)
{
  CAMLassert (root);
  return (value *)root;
}

// hot path
static inline void boxroot_delete_classified(boxroot root, class class)
{
  value *cell = (value *)root;
  if (class != UNTRACKED) {
    free_boxroot(cell);
  } else {
    free(cell);
  }
}

void boxroot_delete(boxroot root)
{
  CAMLassert(root);
  boxroot_delete_classified(root, classify_boxroot(root));
}

void boxroot_modify(boxroot *root, value new_value)
{
  CAMLassert(*root);
  class old_class = classify_boxroot(*root);
  class new_class = classify_value(new_value);

  if (old_class == new_class
      || (old_class == YOUNG && new_class == OLD)) {
    // No need to reallocate
    value *cell = (value *)*root;
    *cell = new_value;
    return;
  }

  boxroot_delete_classified(*root, old_class);
  *root = boxroot_create_classified(new_value, new_class);
  // Note: *root can be NULL, which must be checked (in Rust, check
  // and panic here).
}
