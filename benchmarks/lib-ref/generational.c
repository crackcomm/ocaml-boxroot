#define CAML_NAME_SPACE
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/gc.h>

#include "abstract_out_of_heap.h"

typedef value ref;

ref generational_ref_create(value v)
{
  value b = alloc_abstract_block();
  *(Block_data(b)) = v;
  caml_register_generational_global_root(Block_data(b));
  return b;
}

value generational_ref_get(ref r)
{
  return *Block_data(r);
}

value generational_ref_modify(ref r, value v)
{
  caml_modify_generational_global_root(Block_data(r), v);
  return Val_unit;
}

value generational_ref_delete(ref r)
{
  caml_remove_generational_global_root(Block_data(r));
  free_abstract_block(r);
  return Val_unit;
}
