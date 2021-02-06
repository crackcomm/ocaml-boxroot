#include <stdlib.h>
#include <assert.h>

#define CAML_NAME_SPACE
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/callback.h>

/* In this module, the choice monad is implemented using
   linked-list of OCaml values, registered as global roots */

#include "linked_list.h"
#include "fake_boxroot.h"

// TODO @gasche: boxroot logic should be moved eventually to its own
// subdirectory, so that it would be easier to locate (and split in
// several files if we want to, etc.) than lost in the middle of my
// over-engineering modular benchmarks.

static value node_val_get(struct node *node) {
    return *(fake_boxroot_get((fake_boxroot) node->hd));
}
static void node_val_set(struct node *node, value v) {
    fake_boxroot_modify((fake_boxroot *) &(node->hd), v);
}

static void node_val_init(struct node *node, value v) {
    node->hd = (void *) fake_boxroot_create(v);
}
static void node_val_free(struct node *node) {
    fake_boxroot_delete((fake_boxroot) node->hd);
}

/* C-style functor applications */
#include "value_list_functor.h"
#include "choice_functor.h"

/* exports to OCaml */
value choice_fbr_map(value f, value li) {
    return choice_map(f, li);
};
value choice_fbr_return(value x) {
    return choice_return(x);
}
value choice_fbr_pair(value la, value lb) {
    return choice_pair(la, lb);
}
value choice_fbr_bind(value f, value li) {
    return choice_bind(f, li);
}
value choice_fbr_fail(value unit) {
    return choice_fail(unit);
}
value choice_fbr_choice(value la, value lb) {
    return choice_choice(la, lb);
}
value choice_fbr_run(value li, value f) {
    return choice_run(li, f);
}
