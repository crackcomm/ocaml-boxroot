#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAML_INTERNALS

#include "fast_boxroot.h"
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

// Allocate immediates with malloc to avoid scanning them? Note: if
// this is shown to matter, one can easily introduce a third ring for
// immediates.
#define IMMEDIATES_OPTIM

typedef void * slot;

#define CHUNK_LOG_SIZE 12 // 4KB
#define CHUNK_SIZE (1 << CHUNK_LOG_SIZE)
#define HEADER_SIZE 4
#define CHUNK_ROOTS_CAPACITY (CHUNK_SIZE / sizeof(slot) - HEADER_SIZE)

#define GET_CHUNK_HEADER(v)                                                    \
  ((struct chunk *)((uintptr_t)v & ~((uintptr_t)CHUNK_SIZE - 1)))

typedef struct chunk {
  struct chunk *prev;
  struct chunk *next;
  slot *free_list;
  size_t free_count;
  // Unoccupied slots are either NULL or a pointer to the next free
  // slot. The null value acts as a terminator: if a slot is null,
  // then all subsequent slots are null (bump pointer optimisation).
  slot roots[CHUNK_ROOTS_CAPACITY];
} chunk;

static_assert(sizeof(chunk) <= CHUNK_SIZE, "bad chunk size");

// Rings of chunks
static chunk *old_chunks = NULL; // Contains only old roots
static chunk *young_chunks = NULL; // Contains only young roots

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

static chunk * get_available_chunk(class class)
{
  // In the absence of immediates optimisation, place the immediates
  // with the old values.
  chunk **chunk_ring = (class == YOUNG) ? &young_chunks : &old_chunks;
  chunk *start_chunk = *chunk_ring;
  if (start_chunk) {
    if (start_chunk->free_count > 0)
      return start_chunk;

    chunk *next_chunk = NULL;

    // Find a chunk with available slots
    for (next_chunk = start_chunk->next;
         next_chunk != start_chunk;
         next_chunk = next_chunk->next) {
      if (next_chunk->free_count > 0) {
        // Rotate the ring, making the chunk with free slots the head
        // TODO: maybe better reordering? @gasche: A natural reordering
        // would be to move all the full chunks that were traversed to
        // the end of the chunk list. But this requires a chunk-list
        // structure that knows about both the first and the last
        // elements.
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
  chunk *chunk = GET_CHUNK_HEADER(v);

  *v = chunk->free_list;
  chunk->free_list = (slot)v;
  chunk->free_count += 1;

  // If none of the roots are being used, and it is not the last pool,
  // we can free it.
  if (chunk->free_count == CHUNK_ROOTS_CAPACITY && chunk->next != chunk) {
    chunk->prev->next = chunk->next;
    chunk->next->prev = chunk->prev;
    if (old_chunks == chunk) old_chunks = chunk->next;
    if (young_chunks == chunk) young_chunks = chunk->next;
    free(chunk);
    // TODO: do not free immediately, keep a few empty pools aside (or
    // trust that the allocator does it, unlikely for such large
    // allocations).
  }
}

// Scanning

static void (*fast_boxroot_prev_scan_roots_hook)(scanning_action);

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
      return ++i;
    }
    if (GET_CHUNK_HEADER(v) != chunk) {
      // The value is an OCaml block (or possibly an immediate whose
      // msbs differ from those of chunk, if the immediates
      // optimisation is turned off).
      (*action)((value)v, (value *) &chunk->roots[i]);
    }
  }
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
  // promote minor chunks
  ring_insert(young_chunks, &old_chunks);
  young_chunks = NULL;
  stats.total_scanning_work_minor += work;
}

static void scan_for_major(scanning_action action)
{
  ++stats.major_collections;
  int work = scan_chunks(action, young_chunks);
  work += scan_chunks(action, old_chunks);
  stats.total_scanning_work_major += work;
}

static void fast_boxroot_scan_roots(scanning_action action)
{
  if (is_minor_scanning(action)) {
    scan_for_minor(action);
  } else {
    scan_for_major(action);
  }
  if (fast_boxroot_prev_scan_roots_hook) {
    (*fast_boxroot_prev_scan_roots_hook)(action);
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
  int scanning_work_minor = average(stats.total_scanning_work_minor, stats.minor_collections);
  int scanning_work_major = average(stats.total_scanning_work_major, stats.major_collections);
  int total_mib = mib_of_chunks(stats.total_alloced_chunks);
  int peak_mib = mib_of_chunks(stats.peak_chunks);
  printf("minor collections: %d\n"
         "major collections: %d\n"
         "work per minor: %d\n"
         "work per major: %d\n"
         "total allocated chunks: %d (%d MiB)\n"
         "peak allocated chunks: %d (%d MiB)\n",
         stats.minor_collections,
         stats.major_collections,
         scanning_work_minor,
         scanning_work_major,
         stats.total_alloced_chunks, total_mib,
         stats.peak_chunks, peak_mib);
}

// Must be called to set the hook
void fast_boxroot_scan_hook_setup()
{
  fast_boxroot_prev_scan_roots_hook = caml_scan_roots_hook;
  caml_scan_roots_hook = fast_boxroot_scan_roots;
}

void fast_boxroot_scan_hook_teardown()
{
  caml_scan_roots_hook = fast_boxroot_prev_scan_roots_hook;
  fast_boxroot_prev_scan_roots_hook = NULL;
  if (do_print_stats) print_stats();
  //TODO: free all chunks
}

// Boxroot API implementation

static class classify_root(value v)
{
#ifdef IMMEDIATES_OPTIM
  if(!Is_block(v)) return UNTRACKED;
#endif
  if(Is_young(v)) return YOUNG;
/*#ifndef NO_NAKED_POINTERS
  if(!Is_in_heap(v)) return UNTRACKED;
#endif*/
  return OLD;
}

static inline boxroot boxroot_create(value init, class class)
{
  value *cell;
  switch (class) {
#ifdef IMMEDIATES_OPTIM
  case UNTRACKED:
    cell = (value *) malloc(sizeof(value));
    break;
#endif
  default:
    cell = alloc_boxroot(class);
  }
  if (cell) *cell = init;
  return (boxroot)cell;
}

boxroot fast_boxroot_create(value init)
{
  return boxroot_create(init, classify_root(init));
}

static inline value * boxroot_get(boxroot root)
{
  return (value *)root;
}

value const * fast_boxroot_get(boxroot root)
{
  CAMLassert(root);
  return boxroot_get(root);
}

static inline void boxroot_delete(boxroot root, class class)
{
  value *cell = boxroot_get(root);
  switch (class) {
#ifdef IMMEDIATES_OPTIM
  case UNTRACKED:
    free(cell);
    break;
#endif
  default:
    free_boxroot(cell);
  }
}

void fast_boxroot_delete(boxroot root)
{
  CAMLassert(root);
#ifdef IMMEDIATES_OPTIM
  boxroot_delete(root, classify_root(*boxroot_get(root)));
#else
  boxroot_delete(root, OLD /* irrelevant */);
#endif
}

void fast_boxroot_modify(boxroot *root, value new_value)
{
  value *old_root = boxroot_get(*root);
  class old_class = classify_root(*old_root);
  class new_class = classify_root(new_value);

  if (old_class == new_class) {
    *old_root = new_value;
    return;
  }

  boxroot_delete(*root, old_class);
  *root = boxroot_create(new_value, new_class);
  // Note: *root can be NULL, which must be checked (in Rust, check
  // and panic here).
}
