#include <caml/mlvalues.h>

#include "linked_list.h"

/* functor arguments */
void node_val_init(struct node *, value);
void node_val_free(struct node *);

/* functor body */
static struct node *cons(value hd, struct node *tl) {
    struct node *cell = malloc(sizeof(struct node));
    if (cell == NULL) exit(5);
    node_val_init(cell, hd);
    cell->tl = tl;
    return cell;
}

static struct node *free_node(struct node *node) {
    assert (node != NULL);
    struct node *next = node->tl;
    node_val_free(node);
    free(node);
    return next;
}
