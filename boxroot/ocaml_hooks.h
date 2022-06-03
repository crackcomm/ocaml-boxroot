#ifndef OCAML_HOOKS_H
#define OCAML_HOOKS_H

#include <caml/version.h>
#include "platform.h"

#ifdef CAML_INTERNALS

#include <caml/mlvalues.h>
#include <caml/roots.h>

#if OCAML_MULTICORE

#define CALL_GC_ACTION(action, data, v, p) action(data, v, p)
#define Add_to_ref_table(dom_st, p)                   \
  Ref_table_add(&dom_st->minor_tables->major_ref, p);

#else // if !OCAML_MULTICORE

#define CALL_GC_ACTION(action, data, v, p) do {       \
    action(v, p);                                     \
    (void)data;                                       \
  } while (0)
#define Add_to_ref_table(dom_st, p) add_to_ref_table(dom_st->ref_table, p)

#endif

typedef void (*boxroot_scanning_callback) (scanning_action action,
                                           int only_young, void *data);

void boxroot_setup_hooks(boxroot_scanning_callback scanning,
                         caml_timing_hook domain_termination);

int boxroot_in_minor_collection();

#if OCAML_MULTICORE

/* FIXME: this needs https://github.com/ocaml/ocaml/pull/11272 */
#define assert_domain_lock_held(dom_id) do {    \
    caml_domain_state *dom_st = Caml_state;         \
    assert(dom_st != NULL && dom_st->id == dom_id); \
  } while (0)

#else

/* FIXME: ad hoc implementation using hooks */
#define assert_domain_lock_held(dom_id) do {} while (0)

#endif // OCAML_MULTICORE

#endif // CAML_INTERNALS

#endif // OCAML_HOOKS_H
