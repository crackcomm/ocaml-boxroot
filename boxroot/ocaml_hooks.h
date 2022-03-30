#ifndef OCAML_HOOKS_H
#define OCAML_HOOKS_H

#ifdef CAML_INTERNALS

#include <caml/mlvalues.h>
#include <caml/minor_gc.h>
#include <caml/roots.h>
#include <caml/version.h>

#if OCAML_VERSION >= 50000
#define OCAML_MULTICORE 1
#else
#define OCAML_MULTICORE 0
#endif

#if OCAML_MULTICORE

#include <stdatomic.h>

#define CALL_GC_ACTION(action, data, v, p) action(data, v, p)

#else

#define CALL_GC_ACTION(action, data, v, p) do {       \
    action(v, p);                                     \
    (void)data;                                       \
  } while (0)

#endif // OCAML_MULTICORE

typedef void (*boxroot_scanning_callback) (scanning_action action, void *data);

void boxroot_setup_hooks(boxroot_scanning_callback f);

int boxroot_in_minor_collection();

#if OCAML_MULTICORE

#define Add_to_ref_table(dom_st, p)                   \
  Ref_table_add(&dom_st->minor_tables->major_ref, p);

#else

#define Add_to_ref_table(dom_st, p) add_to_ref_table(dom_st->ref_table, p)

#endif // OCAML_MULTICORE

#endif // CAML_INTERNALS

#endif // OCAML_HOOKS_H
