#ifndef GENERIC_LIST
#define GENERIC_LIST

struct node {
    void *hd;
    struct node *tl;
};

static struct node *replace_tail(struct node *xs, struct node *tail) {
    if (xs == NULL) {
        return tail;
    } else {
        struct node *cur = xs;
        while (cur->tl != NULL) { cur = cur->tl; };
        cur->tl = tail;
        return xs;
    }
}

#endif // GENERIC_LIST
