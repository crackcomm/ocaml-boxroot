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

#ifdef BOXROOT_STATS
static const int do_print_stats = 1;
#else
static const int do_print_stats = 0;
#endif

typedef void * slot;

#define CHUNK_LOG_SIZE 12 // 4KB
#define CHUNK_SIZE (1 << CHUNK_LOG_SIZE)
#define HEADER_SIZE 5
#define CHUNK_ROOTS_CAPACITY (CHUNK_SIZE / sizeof(slot) - HEADER_SIZE)
#define LOW_CAPACITY_THRESHOLD 50 // 50% capacity before promoting a
                                  // young chunk.

typedef struct chunk {
  struct chunk *prev;
  struct chunk *next;
  slot *free_list;
  size_t free_count;
  // Unoccupied slots are either NULL or a pointer to the next free
  // slot. The null value acts as a terminator: if a slot is null,
  // then all subsequent slots are null (bump pointer optimisation).
  size_t capacity; // Number of non-null slots, updated at the end of
                   // each scan.
  slot roots[CHUNK_ROOTS_CAPACITY];
} chunk;

static_assert(sizeof(chunk) <= CHUNK_SIZE, "bad chunk size");

static inline chunk * get_chunk_header(slot v)
{
  return (chunk *)((uintptr_t)v & ~((uintptr_t)CHUNK_SIZE - 1));
}

// Rings of chunks
static chunk *old_chunks = NULL; // Contains only roots pointing to
                                 // the major heap
static chunk *young_chunks = NULL; // Contains roots pointing to the
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
  int total_alloced_chunks;
  int live_chunks;
  int peak_chunks;
} stats; // zero-initialized

static chunk * alloc_chunk()
{
  ++stats.total_alloced_chunks;
  ++stats.live_chunks;
  if (stats.live_chunks > stats.peak_chunks) stats.peak_chunks = stats.live_chunks;

  chunk *out = aligned_alloc(CHUNK_SIZE, CHUNK_SIZE); // TODO: not portable

  if (out == NULL) return NULL;

  out->prev = out->next = out;
  out->free_list = out->roots;
  out->free_count = CHUNK_ROOTS_CAPACITY;
  memset(out->roots, 0, sizeof(out->roots));

  return out;
}

// insert [source] in front of [*target]
static void ring_insert(chunk *source, chunk **target)
{
  chunk *old = *target;
  if (old == NULL) {
    *target = source;
  } else {
    chunk *last = old->prev;
    last->next = source;
    source->prev->next = old;
    old->prev = source->prev;
    source->prev = last;
    *target = young_chunks;
  }
}

// remove the first element from [*target] and return it
static chunk *ring_pop(chunk **target)
{
  chunk *head = *target;
  if (head->next == head) {
    *target = NULL;
    return head;
  }
  head->prev->next = head->next;
  head->next->prev = head->prev;
  *target = head->next;
  head->next = head;
  head->prev = head;
  return head;
}

static chunk * get_available_chunk(class class)
{
  // If there was no optimisation for immediates, we could always place
  // the immediates with the old values (be careful about NULL in
  // naked-pointers mode, though).
  chunk **chunk_ring = (class == YOUNG) ? &young_chunks : &old_chunks;
  chunk *start_chunk = *chunk_ring;
  if (start_chunk) {
    if (start_chunk->free_count > 0)
      return start_chunk;

    chunk *next_chunk = NULL;

    // Find a chunk with available slots
    // TODO: maybe better lookup by putting the more empty chunks in the front
    // during scanning.
    for (next_chunk = start_chunk->next;
         next_chunk != start_chunk;
         next_chunk = next_chunk->next) {
      if (next_chunk->free_count > 0) {
        // Rotate the ring, making the chunk with free slots the head
        *chunk_ring = next_chunk;
        return next_chunk;
      }
    }
  }

  // None found, add a new chunk at the start
  chunk *new_chunk = alloc_chunk();
  if (new_chunk == NULL) return NULL;
  ring_insert(new_chunk, chunk_ring);

  return new_chunk;
}


// Allocation, deallocation

static value * alloc_boxroot(class class)
{
  CAMLassert(class != UNTRACKED);
  chunk *chunk = get_available_chunk(class);
  if (chunk == NULL) return NULL;
  // TODO Latency: bound the number of young roots alloced at each
  // minor collection by scheduling a minor collection.

  slot *root = chunk->free_list;
  chunk->free_count -= 1;

  slot v = *root;
  // root contains either a pointer to the next free slot or NULL
  // if it is NULL we just increase the free_list pointer to the next
  if (v == NULL) {
    chunk->free_list += 1;
  } else {
    // root contains a pointer to the next free slot inside `roots`
    chunk->free_list = (slot *)v;
  }

  return (value *)root;
}

static void free_boxroot(value *root)
{
  slot *v = (slot *)root;
  chunk *c = get_chunk_header(v);

  *v = c->free_list;
  c->free_list = (slot)v;
  c->free_count += 1;

  // If none of the roots are being used, and it is not the last pool,
  // we can free it.
  if (c->free_count == CHUNK_ROOTS_CAPACITY && c->next != c) {
    chunk *hd = ring_pop(&c);
    if (old_chunks == hd) old_chunks = c;
    else if (young_chunks == hd) young_chunks = c;
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

static int scan_chunk(scanning_action action, chunk * chunk)
{
  int i = 0;
  for (; i < CHUNK_ROOTS_CAPACITY; ++i) {
    slot v = chunk->roots[i];
    if (v == NULL) {
      // We can skip the rest if the pointer value is NULL.
      chunk->capacity = i;
      return ++i;
    }
    if (get_chunk_header(v) != chunk) {
      // The value is an OCaml block (or possibly an immediate whose
      // msbs differ from those of [chunk], if the immediates
      // optimisation were to be turned off).
      (*action)((value)v, (value *) &chunk->roots[i]);
    }
  }
  chunk->capacity = i;
  return i;
}

static int scan_chunks(scanning_action action, chunk * start_chunk)
{
  int work = 0;
  if (start_chunk == NULL) return work;
  work += scan_chunk(action, start_chunk);
  for (chunk *chunk = start_chunk->next; chunk != start_chunk; chunk = chunk->next) {
    work += scan_chunk(action, chunk);
  }
  return work;
}

static void scan_for_minor(scanning_action action)
{
  ++stats.minor_collections;
  if (young_chunks == NULL) return;
  int work = scan_chunks(action, young_chunks);
  stats.total_scanning_work_minor += work;
  // promote minor chunks
  chunk *new_young_chunk = NULL;
  if ((young_chunks->capacity * 100 / CHUNK_ROOTS_CAPACITY) <=
      LOW_CAPACITY_THRESHOLD)
    new_young_chunk = ring_pop(&young_chunks);
  if (young_chunks != NULL)
      ring_insert(young_chunks, &old_chunks);
  // allocate the new young chunk lazily
  young_chunks = new_young_chunk;
}

static void scan_for_major(scanning_action action)
{
  ++stats.major_collections;
  int work = scan_chunks(action, young_chunks);
  work += scan_chunks(action, old_chunks);
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

static int mib_of_chunks(int count)
{
  int log_per_chunk = CHUNK_LOG_SIZE - 20;
  if (log_per_chunk >= 0) return count << log_per_chunk;
  if (log_per_chunk < 0) return count >> -log_per_chunk;
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
  int total_mib = mib_of_chunks(stats.total_alloced_chunks);
  int peak_mib = mib_of_chunks(stats.peak_chunks);

  if (scanning_work_minor == 0
      && scanning_work_major == 0
      && stats.total_alloced_chunks == 0)
    return;

  printf("work per minor: %d\n"
         "work per major: %d\n"
         "total allocated chunks: %d (%d MiB)\n"
         "peak allocated chunks: %d (%d MiB)\n",
         scanning_work_minor,
         scanning_work_major,
         stats.total_alloced_chunks, total_mib,
         stats.peak_chunks, peak_mib);
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
  //TODO: free all chunks
}

// Boxroot API implementation

static class classify_value(value v)
{
  if(!Is_block(v)) return UNTRACKED;
  if(Is_young(v)) return YOUNG;
#ifndef NO_NAKED_POINTERS
  if(!Is_in_heap(v)) return UNTRACKED;
#endif
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
