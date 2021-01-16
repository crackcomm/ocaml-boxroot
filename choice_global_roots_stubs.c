#include <stdlib.h>
#include <assert.h>

#define CAML_NAME_SPACE
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/callback.h>

/* this module implements the Choice monad as an abstract
   block pointing to a linked-list of global roots. */
struct node {
    value hd;
    struct node *tl;
};

enum liveness {
    ALIVE,
    DEAD
};

value fresh(struct node *xs) {
    value li = caml_alloc_small(2, Abstract_tag);
    Field(li, 0) = (value)ALIVE;
    Field(li, 1) = (value)xs;
    return li;
}

struct node *consume(value li) {
    assert (Field(li, 0) == ALIVE);
    Field(li, 0) = DEAD;
    return (struct node *)Field(li, 1);
}

struct node *cons(value hd, struct node *tl) {
    struct node *cell = malloc(sizeof(struct node));
    if (cell == NULL) exit(5);
    cell->hd = hd;
    caml_register_global_root(&(cell->hd));
    cell->tl = tl;
    return cell;
}

value get_node_val(struct node *node) {
    return node->hd;
}
void set_node_val(struct node *node, value v) {
    node->hd = v;
}


struct node *replace_tail(struct node *xs, struct node *tail) {
    if (xs == NULL) {
        return tail;
    } else {
        struct node *cur = xs;
        while (cur->tl != NULL) { cur = cur->tl; };
        cur->tl = tail;
        return xs;
    }
}

struct node *free_node(struct node *node) {
    assert (node != NULL);
    struct node *next = node->tl;
    caml_remove_global_root(&(node->hd));
    free(node);
    return next;
}

void free_list(struct node *li) {
    while (li != NULL) {
        li = free_node(li);
    }
}

value choice_gr_map(value f, value li) {
    CAMLparam2 (f, li);
    CAMLlocal2 (res, y);
    struct node *xs = consume(li);

    /* in-place map! */
    for (struct node *cur = xs; cur != NULL; cur = cur->tl) {
        y = caml_callback(f, get_node_val(cur));
        set_node_val(cur, y);
    }
    res = fresh(xs);
    CAMLreturn (res);
}

value choice_gr_return(value x) {
    CAMLparam1 (x);
    CAMLlocal1 (res);

    struct node *xs = cons(x, NULL);

    res = fresh(xs);
    CAMLreturn (res);
}

value choice_gr_pair(value la, value lb) {
    CAMLparam2 (la, lb);
    CAMLlocal2 (res, pair);
    struct node *xsa = consume(la);
    struct node *xsb = consume(lb);
    struct node *acc = NULL;

    for (struct node *cura = xsa; cura != NULL; cura = cura->tl) {
        for (struct node *curb = xsb; curb != NULL; curb = curb->tl) {
            pair = caml_alloc_small(2, 0);
            Field(pair, 0) = get_node_val(cura);
            Field(pair, 1) = get_node_val(curb);

            acc = cons(pair, acc);
        }
    }
    free_list(xsa);
    free_list(xsb);

    res = fresh(acc);
    CAMLreturn (res);
}

value choice_gr_bind(value f, value li) {
    CAMLparam2 (li, f);
    CAMLlocal2 (res, ys);
    struct node *acc = NULL;

    for (struct node *xs = consume(li); xs != NULL; xs = free_node(xs)) {
        ys = caml_callback(f, get_node_val(xs));
        acc = replace_tail(consume(ys), acc);
    }

    res = fresh(acc);
    CAMLreturn (res);
}

value choice_gr_fail(value unit) {
    return fresh(NULL);
}

value choice_gr_choice(value la, value lb) {
    CAMLparam2 (la, lb);
    CAMLlocal1 (res);

    struct node *xsa = consume(la);
    struct node *xsb = consume(lb);
    struct node *xs = replace_tail(xsa, xsb);

    res = fresh(xs);
    CAMLreturn (res);
}

value choice_gr_run(value li, value f) {
    CAMLparam2 (li, f);

    for (struct node *xs = consume(li); xs != NULL; xs = free_node(xs)) {
        (void) caml_callback(f, get_node_val(xs));
    }

    CAMLreturn (Val_unit);
}
