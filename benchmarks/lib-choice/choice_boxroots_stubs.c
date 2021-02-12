#include <stdlib.h>
#include <assert.h>

#define CAML_NAME_SPACE
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/callback.h>

/* In this module, the choice monad is implemented using
   linked-list of OCaml values, stored in fast boxroots */

#include "linked_list.h"
#include "boxroot/boxroot.h"

static value node_val_get(struct node *node) {
    return *(boxroot_get((boxroot) node->hd));
}
static void node_val_set(struct node *node, value v) {
    boxroot_modify((boxroot *) &(node->hd), v);
}

static void node_val_init(struct node *node, value v) {
    node->hd = (void *) boxroot_create(v);
}
static void node_val_free(struct node *node) {
    boxroot_delete((boxroot) node->hd);
}

/* C-style functor applications */
#include "value_list_functor.h"
#include "choice_functor.h"

/* exports to OCaml */
value choice_br_map(value f, value li) {
    return choice_map(f, li);
};
value choice_br_return(value x) {
    return choice_return(x);
}
value choice_br_pair(value la, value lb) {
    return choice_pair(la, lb);
}
value choice_br_bind(value f, value li) {
    return choice_bind(f, li);
}
value choice_br_fail(value unit) {
    return choice_fail(unit);
}
value choice_br_choice(value la, value lb) {
    return choice_choice(la, lb);
}
value choice_br_run(value li, value f) {
    return choice_run(li, f);
}
