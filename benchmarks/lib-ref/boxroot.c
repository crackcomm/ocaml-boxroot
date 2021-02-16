#define CAML_NAME_SPACE
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <assert.h>

#include "../../boxroot/boxroot.h"

typedef value ref;

#define Boxroot_val(r) (boxroot*)&(Field(r,0))

ref boxroot_ref_create(value v) {
    boxroot b = boxroot_create(v);
    value r = caml_alloc_small(1, Abstract_tag);
    *Boxroot_val(r) = b;
    return r;
}

value boxroot_ref_get(ref r) {
    return boxroot_get(*Boxroot_val(r));
}

value boxroot_ref_delete(ref r) {
    boxroot_delete(*Boxroot_val(r));
    return Val_unit;
}

value boxroot_ref_modify(ref r, value v) {
    boxroot_modify(Boxroot_val(r), v);
    return Val_unit;
}

value boxroot_ref_setup(value unit) {
    int ret = boxroot_setup();
    assert(ret == 1);
    return Val_unit;
}

value boxroot_ref_teardown(value unit) {
    boxroot_teardown();
    return Val_unit;
}
