/* functor arguments */
value node_val_get(struct node *);
void node_val_set(struct node *, value);

struct node *cons(value, struct node *);
struct node *free_node(struct node *);

/* functor body */

/* auxiliary list functions */
static void free_list(struct node *li) {
    while (li != NULL) {
        li = free_node(li);
    }
}

/* auxiliary value functions */
#include "abstract_value.h"
static value fresh(struct node *li) {
    return abstract_alloc((void *) li);
}
static struct node *consume(value abstract) {
    return (struct node*) abstract_free(abstract);
}

/* the choice monad implementation */
static value choice_map(value f, value li) {
    CAMLparam2 (f, li);
    CAMLlocal2 (res, y);
    struct node *xs = consume(li);

    /* in-place map! */
    for (struct node *cur = xs; cur != NULL; cur = cur->tl) {
        y = caml_callback(f, node_val_get(cur));
        node_val_set(cur, y);
    }
    res = fresh(xs);
    CAMLreturn (res);
}

static value choice_return(value x) {
    CAMLparam1 (x);
    CAMLlocal1 (res);

    struct node *xs = cons(x, NULL);

    res = fresh(xs);
    CAMLreturn (res);
}

static value choice_pair(value la, value lb) {
    CAMLparam2 (la, lb);
    CAMLlocal2 (res, pair);
    struct node *xsa = consume(la);
    struct node *xsb = consume(lb);
    struct node *acc = NULL;

    for (struct node *cura = xsa; cura != NULL; cura = cura->tl) {
        for (struct node *curb = xsb; curb != NULL; curb = curb->tl) {
            pair = caml_alloc_small(2, 0);
            Field(pair, 0) = node_val_get(cura);
            Field(pair, 1) = node_val_get(curb);

            acc = cons(pair, acc);
        }
    }
    free_list(xsa);
    free_list(xsb);

    res = fresh(acc);
    CAMLreturn (res);
}

static value choice_bind(value f, value li) {
    CAMLparam2 (li, f);
    CAMLlocal2 (res, ys);
    struct node *acc = NULL;

    for (struct node *xs = consume(li); xs != NULL; xs = free_node(xs)) {
        ys = caml_callback(f, node_val_get(xs));
        acc = replace_tail(consume(ys), acc);
    }

    res = fresh(acc);
    CAMLreturn (res);
}

static value choice_fail(value unit) {
    return fresh(NULL);
}

static value choice_choice(value la, value lb) {
    CAMLparam2 (la, lb);
    CAMLlocal1 (res);

    struct node *xsa = consume(la);
    struct node *xsb = consume(lb);
    struct node *xs = replace_tail(xsa, xsb);

    res = fresh(xs);
    CAMLreturn (res);
}

static value choice_run(value li, value f) {
    CAMLparam2 (li, f);

    for (struct node *xs = consume(li); xs != NULL; xs = free_node(xs)) {
        (void) caml_callback(f, node_val_get(xs));
    }

    CAMLreturn (Val_unit);
}
