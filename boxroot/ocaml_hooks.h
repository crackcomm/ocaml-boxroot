#ifndef OCAML_HOOKS_H
#define OCAML_HOOKS_H

#include <caml/version.h>

#if OCAML_VERSION >= 50000
#define OCAML_MULTICORE 1
#else
#define OCAML_MULTICORE 0
#endif

#if OCAML_MULTICORE

/* We currently rely on OCaml 5.0 having a max number of domains; this
   is checked for consistency. */
#define Num_domains 128
#define Domain_id (Caml_state->id)

#else

#define Num_domains 1
#define Domain_id 0

#endif // OCAML_MULTICORE

#ifdef CAML_INTERNALS

#include <caml/mlvalues.h>
#include <caml/minor_gc.h>
#include <caml/roots.h>

#if OCAML_MULTICORE

#include <stdatomic.h>

#define CALL_GC_ACTION(action, data, v, p) action(data, v, p)
#define Add_to_ref_table(dom_st, p)                   \
  Ref_table_add(&dom_st->minor_tables->major_ref, p);

#define BOXROOT_USE_MUTEX 1

#else // if !OCAML_MULTICORE

#define CALL_GC_ACTION(action, data, v, p) do {       \
    action(v, p);                                     \
    (void)data;                                       \
  } while (0)
#define Add_to_ref_table(dom_st, p) add_to_ref_table(dom_st->ref_table, p)

#if defined(ENABLE_BOXROOT_MUTEX) && (ENABLE_BOXROOT_MUTEX == 1)
#define BOXROOT_USE_MUTEX 1
#else
#define BOXROOT_USE_MUTEX 0
#endif // ENABLE_BOXROOT_MUTEX

#endif // OCAML_MULTICORE

typedef void (*boxroot_scanning_callback) (scanning_action action,
                                           int only_young, void *data);

void boxroot_setup_hooks(boxroot_scanning_callback scanning,
                         caml_timing_hook domain_termination);

int boxroot_in_minor_collection();

void assert_domain_lock_held(int dom_id);

#endif // CAML_INTERNALS

#endif // OCAML_HOOKS_H
