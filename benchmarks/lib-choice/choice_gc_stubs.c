#define CAML_NAME_SPACE
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/callback.h>

/* this module implements the Choice monad using OCaml lists handled by the OCaml GC. */

value choice_gc_map(value f, value xs) {
    CAMLparam2 (f, xs);
    CAMLlocal3 (acc, cell, y);

    acc = Val_emptylist;
    for (; xs != Val_emptylist; xs = Field(xs, 1)) {
        y = caml_callback(f, Field(xs, 0));
        cell = caml_alloc_small(2, Tag_cons);
        Field(cell, 0) = y;
        Field(cell, 1) = acc;
        acc = cell;
    }

    CAMLreturn (acc);
}


value choice_gc_return(value x) {
    CAMLparam1 (x);
    CAMLlocal1 (res);

    res = caml_alloc_small(2, Tag_cons);
    Field(res, 0) = x;
    Field(res, 1) = Val_emptylist;

    CAMLreturn (res);
}


value choice_gc_pair(value la, value lb) {
    CAMLparam2 (la, lb);
    CAMLlocal5 (cura, curb, acc, cell, pair);

    acc = Val_emptylist;
    for (cura = la; cura != Val_emptylist; cura = Field(cura, 1)) {
        for (curb = lb; curb != Val_emptylist; curb = Field(cura, 1)) {
            pair = caml_alloc_small(2, 0);
            Field(pair, 0) = Field(cura, 0);
            Field(pair, 1) = Field(curb, 0);

            cell = caml_alloc_small(2, Tag_cons);
            Field(cell, 0) = pair;
            Field(cell, 1) = acc;
            acc = cell;
        }
    }

    CAMLreturn (acc);
}

value choice_gc_bind(value f, value xs) {
    CAMLparam2 (xs, f);
    CAMLlocal3 (ys, acc, cell);

    acc = Val_emptylist;
    for (; xs != Val_emptylist; xs = Field(xs, 1)) {
        ys = caml_callback(f, Field(xs, 0));
        for (; ys != Val_emptylist; ys = Field(ys, 1)) {
            cell = caml_alloc_small(2, Tag_cons);
            Field(cell, 0) = Field(ys, 0);
            Field(cell, 1) = acc;
            acc = cell;
        }
    }

    CAMLreturn (acc);
}

value choice_gc_fail(value unit) {
    return Val_emptylist;
}

value choice_gc_choice(value la, value lb) {
    CAMLparam2 (la, lb);
    CAMLlocal1 (cell);

    for (; la != Val_emptylist; la = Field(la, 1)) {
        cell = caml_alloc_small(2, Tag_cons);
        Field(cell, 0) = Field(la, 0);
        Field(cell, 1) = lb;
        lb = cell;
    }

    CAMLreturn (lb);
}

value choice_gc_run(value li, value f) {
    CAMLparam2 (li, f);

    for (; li != Val_emptylist; li = Field(li, 1)) {
        caml_callback(f, Field(li, 0));
    }

    CAMLreturn (Val_unit);
}
