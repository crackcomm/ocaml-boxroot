#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAML_INTERNALS

#include "boxroot.h"
#include <caml/roots.h>
#include <caml/minor_gc.h>

#ifdef __APPLE__
static void *aligned_alloc(size_t alignment, size_t size) {
  void *memptr = NULL;
  posix_memalign(&memptr, alignment, size);
  return memptr;
}
#endif

static const int do_print_stats = 1;

#define POOL_LOG_SIZE 12 // 4KB
#define POOL_SIZE (1 << POOL_LOG_SIZE)
#define LOW_CAPACITY_THRESHOLD 50 // 50% capacity before promoting a
                                  // young pool.

typedef void * slot;

struct header {
  struct pool *prev;
  struct pool *next;
  slot *free_list;
  size_t free_count;
  // Unoccupied slots are either NULL or a pointer to the next free
  // slot. The null value acts as a terminator: if a slot is null,
  // then all subsequent slots are null (bump pointer optimisation).
  size_t capacity; // Number of non-null slots, updated at the end of
                   // each scan.
};

#define POOL_ROOTS_CAPACITY ((POOL_SIZE - sizeof(struct header)) / sizeof(slot))

typedef struct pool {
  struct header hd;
  slot roots[POOL_ROOTS_CAPACITY];
} pool;

static_assert(sizeof(pool) == POOL_SIZE, "bad pool size");

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
  int live_pools;
  int peak_pools;
} stats; // zero-initialized

static pool * alloc_pool()
{
  ++stats.total_alloced_pools;
  ++stats.live_pools;
  if (stats.live_pools > stats.peak_pools) stats.peak_pools = stats.live_pools;

  pool *out = aligned_alloc(POOL_SIZE, POOL_SIZE); // TODO: not portable

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
static pool *ring_pop(pool **target)
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

static pool * get_available_pool(class class)
{
  // If there was no optimisation for immediates, we could always place
  // the immediates with the old values (be careful about NULL in
  // naked-pointers mode, though).
  pool **pool_ring = (class == YOUNG) ? &young_pools : &old_pools;
  pool *start_pool = *pool_ring;
  if (start_pool) {
    if (start_pool->hd.free_count > 0)
      return start_pool;

    pool *next_pool = NULL;

    // Find a pool with available slots
    // TODO: maybe better lookup by putting the more empty pools in the front
    // during scanning.
    for (next_pool = start_pool->hd.next;
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


// Allocation, deallocation

static value * alloc_boxroot(class class)
{
  CAMLassert(class != UNTRACKED);
  pool *pool = get_available_pool(class);
  if (pool == NULL) return NULL;
  // TODO Latency: bound the number of young roots alloced at each
  // minor collection by scheduling a minor collection.

  slot *root = pool->hd.free_list;
  pool->hd.free_count--;

  slot v = *root;
  // root contains either a pointer to the next free slot or NULL
  // if it is NULL we just increase the free_list pointer to the next
  if (v == NULL) {
    pool->hd.free_list++;
  } else {
    // root contains a pointer to the next free slot inside `roots`
    pool->hd.free_list = (slot *)v;
  }

  return (value *)root;
}

static void free_boxroot(value *root)
{
  slot *v = (slot *)root;
  pool *c = get_pool_header(v);

  *v = c->hd.free_list;
  c->hd.free_list = (slot)v;
  c->hd.free_count++;

  // If none of the roots are being used, and it is not the last pool,
  // we can free it.
  if (c->hd.free_count == POOL_ROOTS_CAPACITY && c->hd.next != c) {
    pool *hd = ring_pop(&c);
    if (old_pools == hd) old_pools = c;
    else if (young_pools == hd) young_pools = c;
    free(hd);
    // TODO: do not free immediately, keep a few empty pools aside (or
    // trust that the allocator does it, unlikely for such large
    // allocations).
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
      pool->hd.capacity = i;
      return ++i;
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

static int scan_pools(scanning_action action, pool * start_pool)
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
  int peak_mib = mib_of_pools(stats.peak_pools);

  if (scanning_work_minor == 0
      && scanning_work_major == 0
      && stats.total_alloced_pools == 0)
    return;

  printf("work per minor: %d\n"
         "work per major: %d\n"
         "total allocated pools: %d (%d MiB)\n"
         "peak allocated pools: %d (%d MiB)\n",
         scanning_work_minor,
         scanning_work_major,
         stats.total_alloced_pools, total_mib,
         stats.peak_pools, peak_mib);
}

// Must be called to set the hook
void boxroot_scan_hook_setup()
{
  boxroot_prev_scan_roots_hook = caml_scan_roots_hook;
  caml_scan_roots_hook = boxroot_scan_roots;
}

void boxroot_scan_hook_teardown()
{
  caml_scan_roots_hook = boxroot_prev_scan_roots_hook;
  boxroot_prev_scan_roots_hook = NULL;
  if (do_print_stats) print_stats();
  //TODO: free all pools
}

// Boxroot API implementation

static class classify_value(value v)
{
  if(v == NULL || !Is_block(v)) return UNTRACKED;
  if(Is_young(v)) return YOUNG;
  return OLD;
}

static class classify_boxroot(boxroot root)
{
    return classify_value(*(value *)root);
}

static inline boxroot boxroot_create_classified(value init, class class)
{
  value *cell;
  switch (class) {
  case UNTRACKED:
    // [init] can be null in naked-pointers mode, handled here.
    cell = (value *) malloc(sizeof(value));
    // TODO: further optim: use a global table instead of malloc for
    // very small values of [init] — for fast variants and and to
    // handle C-side NULLs in no-naked-pointers mode if desired.
    break;
  default:
    cell = alloc_boxroot(class);
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

static inline void boxroot_delete_classified(boxroot root, class class)
{
  value *cell = (value *)root;
  switch (class) {
  case UNTRACKED:
    free(cell);
    break;
  default:
    free_boxroot(cell);
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
