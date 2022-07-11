#define CAML_NAME_SPACE
#include <caml/mlvalues.h>
#include <caml/fail.h>
#include <assert.h>
#include <locale.h>

typedef value ref;

#define CONCAT(g,f) g ## f
#define MY2(g,f) CONCAT(g,f)
#define MY(f) MY2(MY_PREFIX, f)

#define Boxroot_val(r) ((MY(boxroot))(r & ~((value)1)))

ref MY(boxroot_ref_create)(value v)
{
  MY(boxroot) b = MY(boxroot_create)(v);
  return (value)b | (value)1;
}

value MY(boxroot_ref_get)(ref r)
{
  return MY(boxroot_get)(Boxroot_val(r));
}

value MY(boxroot_ref_delete)(ref r)
{
  MY(boxroot_delete)(Boxroot_val(r));
  return Val_unit;
}

value MY(boxroot_ref_modify)(value a, value i, value v)
{
  volatile value *r = &Field(a, Long_val(i));
  MY(boxroot) b = Boxroot_val(*r);
  MY(boxroot_modify)(&b, v);
  /* Replacing an immediate with an immediate */
  *r = (value)b | (value)1;
  return Val_unit;
}

value MY(boxroot_ref_setup)(value unit)
{
  if (!MY(boxroot_setup)()) caml_failwith("boxroot_scan_hook_setup");
  return unit;
}

value MY(boxroot_stats)(value unit)
{
  char *old_locale = setlocale(LC_NUMERIC, NULL);
  setlocale(LC_NUMERIC, "en_US.UTF-8");
  MY(boxroot_print_stats)();
  setlocale(LC_NUMERIC, old_locale);
  return unit;
}

value MY(boxroot_ref_teardown)(value unit)
{
  MY(boxroot_teardown)();
  return unit;
}
