#define CAML_NAME_SPACE
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/fail.h>
#include <caml/callback.h>
#include <assert.h>
#include <locale.h>

#include "../boxroot/boxroot.h"

extern value caml_equal(value, value);

value local_fixpoint(value f, value x) {
  CAMLparam2(f, x);
  CAMLlocal1(y);
  y = caml_callback(f,x);
  if (Double_val(x) == Double_val(y)) {
    CAMLreturn(y);
  } else {
    CAMLreturn(local_fixpoint(f, y));
  }
}

#define BOX(v) boxroot_create(v)
#define GET(b) boxroot_get(b)
#define DROP(b) boxroot_delete(b)

boxroot boxroot_fixpoint_rooted(boxroot f, boxroot x) {
  boxroot y = BOX(caml_callback(GET(f), GET(x)));
  if (Double_val(GET(x)) == Double_val(GET(y))) {
    DROP(f);
    DROP(x);
    return y;
  } else {
    DROP(x);
    return boxroot_fixpoint_rooted(f, y);
  }
}

value boxroot_fixpoint(value f, value x) {
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
