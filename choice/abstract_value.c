#include <caml/mlvalues.h>
#include <caml/alloc.h>
#include <assert.h>

#include "abstract_value.h"

value abstract_alloc(void *x) {
    value v = caml_alloc_small(2, Abstract_tag);
    Field(v, 0) = (value)ALIVE;
    Field(v, 1) = (value)x;
    return v;
}

/* the abstract block is managed by the OCaml GC,
   so we do not actually free the memory. */
void *abstract_free(value v) {
    assert (Field(v, 0) == ALIVE);
    Field(v, 0) = DEAD;
    return (void *) Field(v, 1);
}
