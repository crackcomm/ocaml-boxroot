## Benchmarking OCaml GC root registration

The small program used in this benchmark computes the set of all
permutations of the list [0; ..; n-1], using a non-determinism monad
represented using (strict) lists. (This is an exponential way to
compute factorial(n) with lots of allocations.)

A reference implementation in OCaml is provided, but point is to
implement the non-determinism monad list in C, with each element of
the list registered as a root to the OCaml GC.

## Implementations

Currently we have implemented:
- `choice_ocaml_persistent`: a reference implementation in OCaml
- `choice_ocaml_ephemeral`: like the previous, with update-in-place
  for lists like the C implementations below
- `choice_gc`: a reference implementation in C,
  using lists managed by the OCaml GC (no per-element root)
- `choice_global_roots`: a global root per element
- `choice_generational_roots`: a generational global root per element
- `choice_boxroots`: like `choice_generational_roots`, but the root is
  movable (malloc'ed)
- `choice_fast_boxroots`: the real thing — like `choice_boxroots`, but
  by rolling out our own allocator for movable roots, and implementing
  a fast scanning of the roots registered with caml_scan_roots_hook.

## Some numbers on my machine

```
$ make run
---
ocaml-persistent: 1.96s
count: 3628800
minor collections: 1116
major collections: 18
---
ocaml-ephemeral: 1.88s
count: 3628800
minor collections: 865
major collections: 18
---
gc: 2.09s
count: 3628800
minor collections: 737
major collections: 18
---
global-roots: 12.05s
count: 3628800
minor collections: 526
major collections: 16
---
generational-global-roots: 4.35s
count: 3628800
minor collections: 526
major collections: 16
---
fake-boxroots: 4.29s
count: 3628800
minor collections: 526
major collections: 16
---
boxroots: 2.04s
count: 3628800
minor collections: 526
major collections: 16
work per minor: 7686
work per major: 567779
total allocated chunks: 8150 (31 MiB)
peak allocated chunks: 8150 (31 MiB)
```

We see that global roots add a large overhead (12s compared to 2s when
using the OCaml GC), which is largely reduced by using generational
global roots (4.3s).

"Fake" boxroots (generational roots made movable by placing them in
their owned malloc'ed value) are exactly as fast as generational
global roots.

Real boxroots (implemented with a custom allocator) are competitive
with implementations that do not use per-element roots.
