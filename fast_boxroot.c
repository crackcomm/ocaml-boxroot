#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAML_INTERNALS

#include "fast_boxroot.h"
#include <caml/roots.h>

#define CHUNK_ROOTS_CAPACITY 252
#define CHUNK_ALIGNMENT (sizeof(struct chunk))
#define GET_CHUNK_HEADER(v)                                                    \
  ((struct chunk *)((uintptr_t)v - ((uintptr_t)v % CHUNK_ALIGNMENT)))

typedef struct chunk {
  struct chunk *prev;
  struct chunk *next;
  value *free_list;
  size_t free_count;

  // Unoccupied slots are either NULL or a pointer to the next free slot
  value roots[CHUNK_ROOTS_CAPACITY];
} chunk;

static chunk *fast_boxroot_chunks = NULL;

static int alloc_aligned_block(void **memptr, size_t alignment, size_t size) {
  // TODO: not portable
  return posix_memalign(memptr, alignment, size);
}

static chunk *alloc_chunk() {
  chunk *out = NULL;

  if (alloc_aligned_block((void **)(&out), CHUNK_ALIGNMENT, sizeof(chunk))) {
    exit(1); // TODO: message or proper handling
  }

  out->prev = out->next = NULL;
  out->free_list = out->roots;
  out->free_count = CHUNK_ROOTS_CAPACITY;
  memset(out->roots, 0, sizeof(out->roots));

  return out;
}

static chunk *get_available_chunk() {
  chunk *next_chunk = NULL;
  int i = 0;

  // Find a chunk with available slots
  for (i = 0, next_chunk = fast_boxroot_chunks; next_chunk;
       next_chunk = next_chunk->next, i++) {
    if (next_chunk->free_count) {
      // Reorder the chunks, making the one with free slots the head
      // TODO: maybe better reordering? doesn't make sense to have
      // next_chunk->next point to a sequence of full chunks
      if (next_chunk != fast_boxroot_chunks) {
        next_chunk->prev->next = next_chunk->next;
        if (next_chunk->next) {
          next_chunk->next->prev = next_chunk->prev;
        }
        next_chunk->next = fast_boxroot_chunks;
        fast_boxroot_chunks->prev = next_chunk;
        fast_boxroot_chunks = next_chunk;
      }
      return next_chunk;
    }
    break;
  }

  // None found, add a new chunk
  chunk *next = fast_boxroot_chunks;
  fast_boxroot_chunks = alloc_chunk();
  fast_boxroot_chunks->next = next;
  if (next) {
    next->prev = fast_boxroot_chunks;
  }

  return fast_boxroot_chunks;
}

static boxroot alloc_boxroot() {
  chunk *chunk = get_available_chunk();

  value *root = chunk->free_list;
  chunk->free_count -= 1;

  // root contains either a pointer to the next free slot or NULL
  // if it is NULL we just increase the free_list pointer to the next
  if (!*root) {
    chunk->free_list += 1;
  } else {
    // root contains a pointer to the next free slot inside `roots`
    chunk->free_list = (value *)*root;
  }

  return (boxroot)root;
}

// Scanning

static void (*fast_boxroot_prev_scan_roots_hook)(scanning_action);

// Is allocated if it points outside the range of the root slots
#define IS_ALLOCATED(header, ptr)                                              \
  (ptr < (void *)&header->roots[0] ||                                          \
   ptr > (void *)&header->roots[CHUNK_ROOTS_CAPACITY - 1])

static void fast_boxroot_scan_roots(scanning_action action) {
  chunk *chunk = NULL;
  int i = 0;

  for (chunk = fast_boxroot_chunks; chunk; chunk = chunk->next) {

    for (i = 0; i < CHUNK_ROOTS_CAPACITY; ++i) {
      value *root = &chunk->roots[i];
      if (!root) {
        // We can skip the rest if the pointer value is NULL
        break;
      } else if (Is_long(*root)) {
        continue;
      }
#ifdef NO_NAKED_POINTERS
      else if (!Is_in_heap_or_young(*(value *)root)) {
        continue;
      }
#endif

      void *ptr = (void *)*root;
      if (IS_ALLOCATED(chunk, ptr)) {
        (*action)(*root, root);
      }
    }
  }

  if (fast_boxroot_prev_scan_roots_hook) {
    (*fast_boxroot_prev_scan_roots_hook)(action);
  }
}

// Must be called to set the hook
void fast_boxroot_scan_hook_setup() {
  fast_boxroot_prev_scan_roots_hook = scan_roots_hook;
  scan_roots_hook = fast_boxroot_scan_roots;
}

void fast_boxroot_scan_hook_teardown() {
  scan_roots_hook = fast_boxroot_prev_scan_roots_hook;
  fast_boxroot_prev_scan_roots_hook = NULL;
}

// Boxroot API implementation

boxroot fast_boxroot_create(value init) {
  boxroot r = alloc_boxroot();
  *(value *)r = init;
  return r;
}

value const *fast_boxroot_get(boxroot root) {
  CAMLassert(root);
  return (value const *)root;
}

void fast_boxroot_delete(boxroot root) {
  CAMLassert(root);
  value *v = (value *)root;
  chunk *chunk = GET_CHUNK_HEADER(v);

  *v = (value)(chunk->free_list);
  chunk->free_list = v;
  chunk->free_count += 1;

  // If this is not the "head" chunk link and none of the roots are being
  // used, we can free it.
  if (chunk->free_count == CHUNK_ROOTS_CAPACITY && chunk->prev) {
    chunk->prev->next = chunk->next;
    if (chunk->next) {
      chunk->next->prev = chunk->prev;
    }
    free(chunk);
  }
}

void fast_boxroot_modify(boxroot *root, value new_value) {
  value *v = (value *)*root;
  CAMLassert(v);
  *v = new_value;
}
