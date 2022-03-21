#define CAML_NAME_SPACE
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/fail.h>
#include <assert.h>
#include <locale.h>

#include "../../dll_boxroot/dll_boxroot.h"

typedef value ref;

#define Dll_Boxroot_val(r) (dll_boxroot*)&(Field(r,0))

ref dll_boxroot_ref_create(value v) {
    dll_boxroot b = dll_boxroot_create(v);
    value r = caml_alloc_small(1, Abstract_tag);
    *Dll_Boxroot_val(r) = b;
    return r;
}

value dll_boxroot_ref_get(ref r) {
    return dll_boxroot_get(*Dll_Boxroot_val(r));
}

value dll_boxroot_ref_delete(ref r) {
    dll_boxroot_delete(*Dll_Boxroot_val(r));
    return Val_unit;
}

value dll_boxroot_ref_modify(ref r, value v) {
    dll_boxroot_modify(Dll_Boxroot_val(r), v);
    return Val_unit;
}

value dll_boxroot_ref_setup(value unit)
{
  if (!dll_boxroot_setup()) caml_failwith("dll_boxroot_scan_hook_setup");
  return unit;
}

value dll_boxroot_stats(value unit)
{
  char *old_locale = setlocale(LC_NUMERIC, NULL);
  setlocale(LC_NUMERIC, "en_US.UTF-8");
  dll_boxroot_print_stats();
  setlocale(LC_NUMERIC, old_locale);
  return unit;
}

value dll_boxroot_ref_teardown(value unit)
{
  dll_boxroot_teardown();
  return unit;
}
