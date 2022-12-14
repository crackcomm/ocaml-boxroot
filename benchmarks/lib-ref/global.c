/* SPDX-License-Identifier: MIT */
#define CAML_NAME_SPACE
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>

#include "tagged_out_of_heap.h"

typedef value ref;

ref global_ref_create(value v)
{
  value b = alloc_tagged_block();
  *(Block_data(b)) = v;
  caml_register_global_root(Block_data(b));
  return b;
}

value global_ref_get(ref r)
{
  return *(Block_data(r));
}

value global_ref_modify(value a, value i, value v)
{
  ref r = Field(a, Long_val(i));
  *Block_data(r) = v;
  return Val_unit;
}

value global_ref_delete(ref r)
{
  caml_remove_global_root(Block_data(r));
  free_tagged_block(r);
  return Val_unit;
}
