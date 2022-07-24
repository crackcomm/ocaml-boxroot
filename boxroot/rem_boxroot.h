/* SPDX-License-Identifier: MIT */
#ifndef REM_BOXROOT_H
#define REM_BOXROOT_H

#include <caml/mlvalues.h>

typedef struct rem_boxroot_private* rem_boxroot;

/* `rem_boxroot_create(v)` allocates a new boxroot initialised to the
   value `v`. This value will be considered as a root by the OCaml GC
   as long as the boxroot lives or until it is modified. A return
   value of `NULL` indicates a failure of allocation of the backing
   store. */
rem_boxroot rem_boxroot_create(value);

/* `rem_boxroot_get(r)` returns the contained value, subject to the usual
   discipline for non-rooted values. `rem_boxroot_get_ref(r)` returns a
   pointer to a memory cell containing the value kept alive by `r`,
   that gets updated whenever its block is moved by the OCaml GC. The
   pointer becomes invalid after any call to `rem_boxroot_delete(r)` or
   `rem_boxroot_modify(&r,v)`. The argument must be non-null. */
inline value rem_boxroot_get(rem_boxroot r) { return *(value *)r; }
inline value const * rem_boxroot_get_ref(rem_boxroot r) { return (value *)r; }

/* `rem_boxroot_delete(r)` deallocates the boxroot `r`. The value is no
   longer considered as a root by the OCaml GC. The argument must be
   non-null. */
void rem_boxroot_delete(rem_boxroot);

/* `rem_boxroot_modify(&r,v)` changes the value kept alive by the boxroot
   `r` to `v`. It is equivalent to the following:
   ```
   rem_boxroot_delete(r);
   r = rem_boxroot_create(v);
   ```
   In particular, the root can be reallocated. However, unlike
   `rem_boxroot_create`, `rem_boxroot_modify` never fails, so `r` is
   guaranteed to be non-NULL afterwards. In addition, `rem_boxroot_modify`
   is more efficient. */
void rem_boxroot_modify(rem_boxroot *, value);


/* The behaviour of the above functions is well-defined only after the
   allocator has been initialised with `rem_boxroot_setup`, which must be
   called after OCaml startup, and before it has released its
   resources with `rem_boxroot_teardown`, which can be called after OCaml
   shutdown. */
int rem_boxroot_setup();
void rem_boxroot_teardown();

/* Show some statistics on the standard output. */
void rem_boxroot_print_stats();

#endif // REM_BOXROOT_H
