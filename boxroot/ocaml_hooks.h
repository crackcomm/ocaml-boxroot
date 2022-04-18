#ifndef OCAML_HOOKS_H
#define OCAML_HOOKS_H

#include <caml/version.h>

#if OCAML_VERSION >= 50000
#define OCAML_MULTICORE 1
#else
#define OCAML_MULTICORE 0
#endif

#ifdef CAML_INTERNALS

#include <caml/mlvalues.h>
#include <caml/minor_gc.h>
#include <caml/roots.h>

#if OCAML_MULTICORE

#include <stdatomic.h>
#include <caml/config.h>
#define CALL_GC_ACTION(action, data, v, p) action(data, v, p)
#define Add_to_ref_table(dom_st, p)                   \
  Ref_table_add(&dom_st->minor_tables->major_ref, p);

#define BOXROOT_USE_MUTEX 1

#else

#define Max_domains 1
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

void boxroot_setup_hooks(boxroot_scanning_callback f);

int boxroot_in_minor_collection();

#endif // CAML_INTERNALS

#endif // OCAML_HOOKS_H
