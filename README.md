# Boxroot: fast movable roots for the OCaml C interface

This repository hosts an experiment with a different root-registration
API for the OCaml garbage collector. The new kind of roots are called
`boxroot` (boxed roots).

The traditional root-registration APIs let users decide which existing
parts of memory should be considered as new roots by the runtime. With
boxroots, it is our allocator, not the user, that decides where these
roots are placed in memory. This extra flexibility allows for a more
efficient implementation.

We provide an implementation of this idea as a standalone C library
([boxroot/](boxroot/) in this repository), as a custom allocator using
OCaml's GC scanning hooks. Our prototype already shows promising
performance in benchmarks.

In addition to better performance, movable roots fit a common use-case
where Ocaml values are placed inside malloc'ed blocks and then
registered as global roots, for instance for insertion in C library
data structures. This pattern appears to be common. Our original
motivation for prototyping boxroots generalises that: it is to propose
an idiomatic manipulation of OCaml roots from Rust, similar to
`Box<T>` pointers.

## Design

Functions to acquire, read, release and modify a `boxroot` are
provided as follows.

```c
boxroot boxroot_create(value);
value boxroot_get(boxroot);
value const * boxroot_get_ref(boxroot);
void boxroot_delete(boxroot);
void boxroot_modify(boxroot *, value);
```

These functions operate in constant time. (This can be compared to the
probabilistic logarithmic time offered by global roots.)

See [boxroot/boxroot.h](boxroot/boxroot.h) for API documentation.

## Benchmarks

To evaluate our experiment, we run various allocation-heavy
benchmarks.

### Implementations

The benchmarks compares various implementation of an OCaml reference
type Ref containing a single value with an imperative interface
(`create`, `get`, `delete`, `modify`):

- `ocaml`: an OCaml implementation using a mutable record, with
  deletion implemented by assigning `()` using Obj.magic.
- `gc`: a C implementation of the previous using
  `caml_alloc_small(1,0)`,
- `boxroot`: a `boxroot` disguised as an immediate (reference
  implementation described further below),
- `global`: a block allocated outside the OCaml heap (disguised as an
  immediate) containing a global root, and
- `generational`: idem, but using a generational
  global root.
- `dll_boxroot`: a variant of `boxroot`, but using a "naive"
  implementation with doubly-linked lists,
- `rem_boxroot`: a variant of `boxroot`, but using a simpler
  implementation using OCaml's remembered set.

The various implementations have similar memory representation, some
on the OCaml heap and some outside of the OCaml heap.

By selecting different implementations of Ref, we can evaluate the
overhead of root registration and scanning for various root
implementations, compared to non-rooting OCaml and C implementations.

### Benchmark information

The figures below are obtained with OCaml 4.12, with CPU AMD Ryzen
5850U.

### Permutations of a list

The small program used in this benchmark computes the set of all
permutations of the list [0; ..; n-1], using a non-determinism monad
represented using (strict) lists. (This is an exponential way to
compute factorial(n) with lots of allocations.)

In our non-determinism monad, each list element goes through the Ref
module that boxes its underlying value, and may be implemented
(through C stubs) as an abstract block (not followed by the GC) whose
value is registered as a GC root.

This benchmark creates a lot of roots alive at the same time.

```
$ make run-perm_count TEST_MORE=1
Benchmark: perm_count
---
gc: 2.17s            
count: 3628800
---
boxroot: 2.05s       
count: 3628800
---
ocaml: 2.14s         
count: 3628800
---
dll_boxroot: 2.15s   
count: 3628800
---
rem_boxroot: 2.04s   
count: 3628800
---
generational: 5.59s  
count: 3628800
---
global: 53.79s       
count: 3628800
---
```

We see that global roots add a large overhead, which is reduced by
using generational global roots. Boxroots outperform generational
global roots, and are competitive with the reference implementations
that do not use roots (ocaml and gc).


### Synthetic benchmark

In this benchmark, we allocate and deallocate values and roots
according to probabilities determined by parameters.

* `N=8`: log_2 of the number of minor generations
* `SMALL_ROOTS=10_000`: the number of small roots allocated (in the
  minor heap) per minor collection,
* `LARGE_ROOTS=20`: the number of large roots allocated (in the major
  heap) per minor collection,
* `SMALL_ROOT_PROMOTION_RATE=0.2`: the survival rate for small roots
  allocated in the current minor heap,
* `LARGE_ROOT_PROMOTION_RATE=1`: the survival rate for large roots
  allocated in the current minor heap,
* `ROOT_SURVIVAL_RATE=0.99`: the survival rate for roots that survived
  a first minor collection,
* `GC_PROMOTION_RATE=0.1`: promotion rate of GC-tracked values,
* `GC_SURVIVAL_RATE=0.5`: survival rate of GC-tracked values.

These settings favour the creation of a lot of roots, most of which
are short-lived. Roots that survive are few, but they are very
long-lived.

```
$ make run-synthetic TEST_MORE=1
Benchmark: synthetic
---
gc: 7.30s            
---
boxroot: 6.18s       
---
ocaml: 7.21s         
---
dll_boxroot: 6.63s   
---
rem_boxroot: 6.04s   
---
generational: 11.29s 
---
global: 16.38s       
---
```

Since the boxroot is directly inside a gc-allocated value, our
benchmarks leave few opportunities for the version using boxroots
outperforming the versions without roots, so the repeatable
outperformance of non-roots versions by the boxroot version in this
benchmark is surprising. It could be explained by the greater cache
locality of pools during scanning.


### Globroot benchmark

This benchmark is adapted from the OCaml testsuite. It exercises the
case where there are about 1024 concurrently-live roots, but only a
couple of young roots are created between two minor collections.

This benchmark tests the case where there are few
concurrently-live roots and little root creation and
modification between two collections. This corresponds to
a common scenario where the FFI is rarely used, except that
this benchmark does not perform any OCaml computations or
allocations (it forces collections to occur very often despite
low GC work), so the cost of root handling is magnified, it
would normally be amortized by OCaml computations.

```
$ make run-globroots TEST_MORE=1
---
gc: 1.37s            
---
boxroot: 1.23s       
---
ocaml: 1.47s         
---
dll_boxroot: 1.05s   
---
rem_boxroot: 1.13s   
---
generational: 1.24s  
---
global: 1.36s        
---
```

In this benchmark, there are about 67000 minor collections and 40000
major collections. List-based implementations perform well, whereas
`boxroot` is slower. In the way it is currently implemented, it can
have to scan on the order of a full memory pool at every minor
collection even if there are only a few young roots, for a pool size
currently chosen large (16KB). Nevertheless, the overhead compared to
`dll_boxroot` is only 2µs per minor collection.

### Local roots benchmark

We designed this benchmark to test the idea of replacing local
roots altogether by boxroots.

Currently, OCaml FFI code uses a "callee-roots" discipline
where each function has to locally "root" each OCaml value
received as argument or used as a temporary paramter, using
the efficient `CAMLparam`, `CAMLlocal`, `CAMLreturn` macros.

Boxroots suggest a "caller-root" approach where callers would
package their OCaml values in boxroots, whose ownership is
passed to the callee. Creating boxroots is slower than
registering local roots, but the caller-root discipline can
avoid re-rooting each value when moving up and down the call
chain, so it should have a performance advantage for deep call
chains.

This benchmark performs a (recursive) fixpoint computation on
OCaml floating-point value from C, with a parameter N that
decides the number of fixpoint iterations necessary, and thus
the length of the C-side call chain.

The local-roots version is as follows:

```c
int compare_val(value x, value y);

value local_fixpoint(value f, value x)
{
  CAMLparam2(f, x);
  CAMLlocal1(y);
  y = caml_callback(f,x);
  if (compare_val(x, y)) {
    CAMLreturn(y);
  } else {
    CAMLreturn(local_fixpoint(f, y));
  }
}
```
where `compare_val` compares the values of `x` and `y`, but introduces
local roots in order to simulate a more complex operation.

The boxroot version is as follows:

```c
value boxroot_fixpoint(value f, value x)
{
  boxroot y = boxroot_fixpoint_rooted(BOX(f), BOX(x));
  value v = GET(y);
  DROP(y);
  return v;
}

#define BOX(v) boxroot_create(v)
#define GET(b) boxroot_get(b)
#define DROP(b) boxroot_delete(b)

int compare_refs(value const *x, value const *y);

boxroot boxroot_fixpoint_rooted(boxroot f, boxroot x)
{
  boxroot y = BOX(caml_callback(GET(f), GET(x)));
  if (compare_refs(GET_REF(x), GET_REF(y))) {
    DROP(f);
    DROP(x);
    return y;
  } else {
    DROP(x);
    return boxroot_fixpoint_rooted(f, y);
  }
}
```
where `compare_refs` does the same work as `compare_val` but does not
need to root its values.

The work is done by `boxroot_fixpoint_rooted`, but we need a
`boxroot_fixpoint` wrapper to go from the callee-roots convention
expected by OCaml `external` declarations to a caller-root convention.
(This wrapper also adds some overhead for small call depths.)

```
Benchmark: local_roots TEST_MORE=1
---
local_roots(ROOT=local       , N=1):    12.31ns
local_roots(ROOT=boxroot     , N=1):    19.44ns
local_roots(ROOT=dll_boxroot , N=1):    30.27ns
local_roots(ROOT=rem_boxroot , N=1):    22.98ns
local_roots(ROOT=generational, N=1):    48.88ns
---
local_roots(ROOT=local       , N=2):    23.35ns
local_roots(ROOT=boxroot     , N=2):    29.09ns
local_roots(ROOT=dll_boxroot , N=2):    44.52ns
local_roots(ROOT=rem_boxroot , N=2):    35.25ns
local_roots(ROOT=generational, N=2):   105.56ns
---
local_roots(ROOT=local       , N=3):    34.63ns
local_roots(ROOT=boxroot     , N=3):    37.63ns
local_roots(ROOT=dll_boxroot , N=3):    60.24ns
local_roots(ROOT=rem_boxroot , N=3):    47.30ns
local_roots(ROOT=generational, N=3):   137.82ns
---
local_roots(ROOT=local       , N=4):    45.30ns
local_roots(ROOT=boxroot     , N=4):    47.75ns
local_roots(ROOT=dll_boxroot , N=4):    72.60ns
local_roots(ROOT=rem_boxroot , N=4):    57.68ns
local_roots(ROOT=generational, N=4):   174.14ns
---
local_roots(ROOT=local       , N=5):    57.73ns
local_roots(ROOT=boxroot     , N=5):    56.59ns
local_roots(ROOT=dll_boxroot , N=5):    90.08ns
local_roots(ROOT=rem_boxroot , N=5):    69.86ns
local_roots(ROOT=generational, N=5):   217.36ns
---
local_roots(ROOT=local       , N=10):   117.16ns
local_roots(ROOT=boxroot     , N=10):   102.86ns
local_roots(ROOT=dll_boxroot , N=10):   166.69ns
local_roots(ROOT=rem_boxroot , N=10):   127.74ns
local_roots(ROOT=generational, N=10):   407.40ns
---
local_roots(ROOT=local       , N=100):  1267.35ns
local_roots(ROOT=boxroot     , N=100):   954.10ns
local_roots(ROOT=dll_boxroot , N=100):  1476.02ns
local_roots(ROOT=rem_boxroot , N=100):  1196.25ns
local_roots(ROOT=generational, N=100):  3726.56ns
---
local_roots(ROOT=local       , N=1000): 13222.97ns
local_roots(ROOT=boxroot     , N=1000):  8940.01ns
local_roots(ROOT=dll_boxroot , N=1000): 14795.19ns
local_roots(ROOT=rem_boxroot , N=1000): 11319.29ns
local_roots(ROOT=generational, N=1000): 35870.84ns
---
```

We see that, for a call depth of 1, the boxroot version is about 60%
slower than the local-roots version. This is a good result: the amount
of computation is very small, and there is an up-front cost for
wrapping the function, so we could initially expect a large overhead
for boxroot over local roots.

The performance advantage of local roots over boxroots disappears with
greater values of N in this micro-benchmark. A high value of N is not
only relevant for deep call chains, but also of functions containing
many calls to functions manipulating ocaml values.

Our conclusions:
- Using boxroots is competitive with local roots.
- It could be beneficial in specific scenarios, for instance when
  traversing large OCaml structures from a foreign language, with many
  function calls.

Furthermore, we envision that with support from the OCaml compiler for
the caller-roots discipline, the initial wrapping could be made
unnecessary.

## Implementation

We implemented a custom allocator that manages fairly standard
freelist-based memory pools, but we make arrangements such that we can
scan these pools efficiently. In standard fashion, the pools are
aligned in such a way that the most significant bits can be used to
identify the pool from the address of their members. Since elements of
the freelist are guaranteed to point only inside the memory pool, and
non-immediate OCaml values are guaranteed to point only outside of the
memory pool, we can identify allocated slots as follows:

```
allocated(slot, pool) ≝ (pool != (slot & ~(1<<N - 2)))
```

N is a parameter determining the size of the pools. The bitmask is
chosen to preserve the least significant bit, so that immediate OCaml
values (with lsb set) are correctly classified.

Scanning is set up by registering a root scanning hook with the OCaml
GC, and done by traversing the pools linearly. An early-exit
optimisation when all roots have been found ensures that programs that
use few roots throughout the life of the program only pay for what
they use.

The memory pools are managed in several rings, distinguished by their
*class* and their *occupancy*. In addition to distinguishing the pools
in use (which need to be scanned) from the pools that are free (and
need not be scanned), the class distinguishes pools according to OCaml
generations. A pool is *young* if and only if it is allowed to contain
pointers to the minor heap. During minor collection, we only need to
scan young pools. At the end of the minor collection, the young pools,
now guaranteed to no longer point to any young value, are promoted
into *old* pools.

In addition to distinguishing pools that are available for allocation
from pools that are (quasi-)full, occupancy distinguishes the old
pools that are quite more empty than others. By this we mean
half-empty. Such pools are considered in priority for *demotion* into
a young pool. (These pool contain major roots, but it is harmless to
scan them during minor collection.) If no such pool is available for
recycling into a young pool, we prefer to allocate a new pool.

This heuristic of choosing pools at least half-empty guarantees that
more than half of the scanning effort during minor collection is
devoted to slots containing young values, or available for the
allocation of young values (disregarding some optimisation in
`boxroot_modify`). This amounts to trading efficiency guarantees of
scanning against a slightly sub-optimal overall occupancy.

Care is taken so that programs that do not allocate any root do not
pay any of the cost.

## Limitations

* Our prototype library uses `posix_memalign`, which currently limits
  its portability on some systems.

* No synchronisation is performed yet in the above benchmarks. In the
  released version, synchronisation is currently implemented with a
  global lock not representative of expected performance. We intend to
  lift this limitation in the future.

* Due to limitations of the GC hook interface, no work has been done
  to scan roots incrementally. Holding a (very!) large number of roots
  at the same time can negatively affect latency.
