#include "fake_boxroot.h"
#include <caml/memory.h>

fake_boxroot fake_boxroot_create(value init)
{
  value * v = (value *) malloc(sizeof(value));
  if (!v) return NULL;
  *v = init;
  caml_register_generational_global_root(v);
  return (fake_boxroot)v;
}

value const * fake_boxroot_get(fake_boxroot r)
{
  CAMLassert(r);
  return (value const *)r;
}

void fake_boxroot_delete(fake_boxroot r)
{
  value * v = (value *) r;
  CAMLassert(v);
  caml_remove_generational_global_root(v);
  free(v);
}

void fake_boxroot_modify(fake_boxroot * r, value new)
{
  value * v = (value *) *r;
  CAMLassert(v);
  caml_modify_generational_global_root(v, new);
}
