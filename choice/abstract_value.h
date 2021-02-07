#ifndef ABSTRACT_VALUE
#define ABSTRACT_VALUE

#include <caml/mlvalues.h>

/* OCaml abstract block holding a C pointer */
enum liveness {
    ALIVE,
    DEAD
};

value abstract_alloc(void *);
void *abstract_free(value);

#endif // ABSTRACT_VALUE
