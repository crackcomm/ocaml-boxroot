#define CAML_NAME_SPACE
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/fail.h>
#include <caml/callback.h>
#include <assert.h>
#include <locale.h>

#include "../boxroot/boxroot.h"

/* a fixpoint-computation function defined using the usual C-FFI style
   for OCaml values, following the "callee roots" convention:
   a function is passed value that may be unrooted, and it is
   responsible for rooting them if it may call the GC. */

int compare_val(value x, value y)
{
  /* Simulate a function that does some actual work. */
  CAMLparam2(x, y);
  CAMLreturn(Double_val(x) == Double_val(y));
}

value local_fixpoint(value f, value x)
{
  CAMLparam2(f, x);
  CAMLlocal1(y);
  y = caml_callback(f,x);
  if (compare_val(x,y)) {
    CAMLreturn(y);
  } else {
    CAMLreturn(local_fixpoint(f, y));
  }
}

/* a different version that uses our 'boxroot' library to implement
   a "caller roots" convention: a function is passed functions that
   have already been rooted into boxroots, and it may itself pass them
   around to its own callee without re-rooting. */
#define BOX(v) boxroot_create(v)
#define GET(b) boxroot_get(b)
#define GET_REF(b) boxroot_get_ref(b)
#define DROP(b) boxroot_delete(b)

int compare_refs(value const *x, value const *y)
{
  /* Simulate a function that does some actual work---nothing to root
     here. */
  return Double_val(*x) == Double_val(*y);
}

boxroot boxroot_fixpoint_rooted(boxroot f, boxroot x)
{
  boxroot y = BOX(caml_callback(GET(f), GET(x)));
  if (compare_refs(GET_REF(x), GET_REF(y))) {
    DROP(f);
    DROP(x);
    return y;
  } else {
    DROP(x);
    return boxroot_fixpoint_rooted(f, y);
  }
}

value boxroot_fixpoint(value f, value x)
{
  boxroot y = boxroot_fixpoint_rooted(BOX(f), BOX(x));
  value v = GET(y);
  DROP(y);
  return v;
}

/* TODO: it is annoying to have to copy this code in several
   libraries, maybe we should provide an OCaml library that simply
   re-exports the boxroot.h interface within OCaml. */
value caml_boxroot_setup(value unit)
{
  if (!boxroot_setup()) caml_failwith("boxroot_scan_hook_setup");
  return unit;
}

value caml_boxroot_teardown(value unit)
{
  boxroot_teardown();
  return unit;
}

value caml_boxroot_stats(value unit)
{
  char *old_locale = setlocale(LC_NUMERIC, NULL);
  setlocale(LC_NUMERIC, "en_US.UTF-8");
  boxroot_print_stats();
  setlocale(LC_NUMERIC, old_locale);
  return unit;
}

/* This is a variation of the caller-root example using "fake
   boxroots", namely malloced blocks containing generational global
   roots. This should be sensibly slower than boxroot, we are using it
   as a "control" that the benchmark makes sense. */
typedef value *genroot;

static inline value GEN_GET(genroot b)
{
  return *b;
}

static inline value const * GEN_GET_REF(genroot b)
{
  return b;
}

static inline genroot GEN_BOX(value v)
{
  value *b = malloc(sizeof(value));
  *b = v;
  caml_register_generational_global_root(b);
  return b;
}

static inline void GEN_DROP(genroot b)
{
  caml_remove_generational_global_root(b);
  free(b);
}

genroot generational_fixpoint_rooted(genroot f, genroot x)
{
  genroot y = GEN_BOX(caml_callback(GEN_GET(f), GEN_GET(x)));
  if (compare_refs(GEN_GET_REF(x), GEN_GET_REF(y))) {
    GEN_DROP(f);
    GEN_DROP(x);
    return y;
  } else {
    GEN_DROP(x);
    return generational_fixpoint_rooted(f, y);
  }
}

value generational_fixpoint(value f, value x)
{
  genroot y = generational_fixpoint_rooted(GEN_BOX(f), GEN_BOX(x));
  value v = GEN_GET(y);
  GEN_DROP(y);
  return v;
}


/* same as the 'boxroot' version, but using 'dll_boxroot' instead */
#include "../dll_boxroot/dll_boxroot.h"

#undef BOX
#undef GET
#undef DROP
#define BOX(v) dll_boxroot_create(v)
#define GET(b) dll_boxroot_get(b)
#define DROP(b) dll_boxroot_delete(b)

dll_boxroot dll_boxroot_fixpoint_rooted(dll_boxroot f, dll_boxroot x) {
  dll_boxroot y = BOX(caml_callback(GET(f), GET(x)));
  if (Double_val(GET(x)) == Double_val(GET(y))) {
    DROP(f);
    DROP(x);
    return y;
  } else {
    DROP(x);
    return dll_boxroot_fixpoint_rooted(f, y);
  }
}

value dll_boxroot_fixpoint(value f, value x) {
  dll_boxroot y = dll_boxroot_fixpoint_rooted(BOX(f), BOX(x));
  value v = GET(y);
  DROP(y);
  return v;
}

value caml_dll_boxroot_setup(value unit)
{
  if (!dll_boxroot_setup()) caml_failwith("dll_boxroot_scan_hook_setup");
  return unit;
}

value caml_dll_boxroot_teardown(value unit)
{
  dll_boxroot_teardown();
  return unit;
}

value caml_dll_boxroot_stats(value unit)
{
  char *old_locale = setlocale(LC_NUMERIC, NULL);
  setlocale(LC_NUMERIC, "en_US.UTF-8");
  dll_boxroot_print_stats();
  setlocale(LC_NUMERIC, old_locale);
  return unit;
}
