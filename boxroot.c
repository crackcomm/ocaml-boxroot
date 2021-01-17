#include "boxroot.h"
#include <caml/memory.h>

boxroot boxroot_create(value init)
{
  value * v = (value *) malloc(sizeof(value));
  if (!v) return NULL;
  *v = init;
  caml_register_generational_global_root(v);
  return (boxroot)v;
}

value const * boxroot_get(boxroot r)
{
  CAMLassert(r);
  return (value const *)r;
}

void boxroot_delete(boxroot r)
{
  value * v = (value *) r;
  CAMLassert(v);
  caml_remove_generational_global_root(v);
  free(v);
}

void boxroot_modify(boxroot * r, value new)
{
  value * v = (value *) *r;
  CAMLassert(v);
  caml_modify_generational_global_root(v, new);
}
