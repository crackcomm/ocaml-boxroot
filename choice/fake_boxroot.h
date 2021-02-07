#ifndef FAKE_BOXROOT_H
#define FAKE_BOXROOT_H

#include <caml/mlvalues.h>

typedef struct fake_boxroot_private* fake_boxroot;

fake_boxroot fake_boxroot_create(value);
value const * fake_boxroot_get(fake_boxroot);
void fake_boxroot_delete(fake_boxroot);
void fake_boxroot_modify(fake_boxroot *, value);

#endif // FAKE_BOXROOT_H
