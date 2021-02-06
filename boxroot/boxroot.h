#ifndef BOXROOT_H
#define BOXROOT_H

#include <caml/mlvalues.h>

typedef struct boxroot_private* boxroot;

boxroot boxroot_create(value);
value const * boxroot_get(boxroot);
void boxroot_delete(boxroot);
void boxroot_modify(boxroot *, value);

#endif // BOXROOT_H
