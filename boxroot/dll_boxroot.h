#ifndef DLL_BOXROOT_H
#define DLL_BOXROOT_H

#include <caml/mlvalues.h>

typedef struct dll_boxroot_private* dll_boxroot;

/* `dll_boxroot_create(v)` allocates a new boxroot initialised to the
   value `v`. This value will be considered as a root by the OCaml GC
   as long as the boxroot lives or until it is modified. A return
   value of `NULL` indicates a failure of allocation of the backing
   store. */
dll_boxroot dll_boxroot_create(value);

/* `dll_boxroot_get(r)` returns the contained value, subject to the usual
   discipline for non-rooted values. `dll_boxroot_get_ref(r)` returns a
   pointer to a memory cell containing the value kept alive by `r`,
   that gets updated whenever its block is moved by the OCaml GC. The
   pointer becomes invalid after any call to `dll_boxroot_delete(r)` or
   `dll_boxroot_modify(&r,v)`. The argument must be non-null. */
value dll_boxroot_get(dll_boxroot);
value const * dll_boxroot_get_ref(dll_boxroot);

/* `dll_boxroot_delete(r)` desallocates the boxroot `r`. The value is no
   longer considered as a root by the OCaml GC. The argument must be
   non-null. */
void dll_boxroot_delete(dll_boxroot);

/* `dll_boxroot_modify(&r,v)` changes the value kept alive by the boxroot
   `r` to `v`. It is equivalent to the following:
   ```
   dll_boxroot_delete(r);
   r = dll_boxroot_create(v);
   ```
   In particular, the root can be reallocated. However, unlike
   `dll_boxroot_create`, `dll_boxroot_modify` never fails, so `r` is
   guaranteed to be non-NULL afterwards. In addition, `dll_boxroot_modify`
   is more efficient. Indeed, the reallocation, if needed, occurs at
   most once between two minor collections. */
void dll_boxroot_modify(dll_boxroot *, value);

/* The behaviour of the above functions is well-defined only after the
   allocator has been initialised with `dll_boxroot_setup`, which must be
   called after OCaml startup, and before it has released its
   resources with `dll_boxroot_teardown`. */
int dll_boxroot_setup();
void dll_boxroot_teardown();

/* Show some statistics on the standard output. */
void dll_boxroot_print_stats();

#endif // DLL_BOXROOT_H
