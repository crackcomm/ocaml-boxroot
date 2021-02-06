#ifndef FAST_BOXROOT_H
#define FAST_BOXROOT_H

#include <caml/mlvalues.h>

typedef struct boxroot_private* boxroot;

boxroot fast_boxroot_create(value);
value const * fast_boxroot_get(boxroot);
void fast_boxroot_delete(boxroot);
void fast_boxroot_modify(boxroot *, value);

#endif // FAST_BOXROOT_H
