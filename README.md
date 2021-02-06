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
$ make bench
time ./perm_count ocaml-persistent 10
3628800
2.74user 0.28system 0:03.03elapsed 99%CPU (0avgtext+0avgdata 715040maxresident)k
0inputs+0outputs (0major+179246minor)pagefaults 0swaps

time ./perm_count ocaml-ephemeral 10
3628800
2.52user 0.25system 0:02.78elapsed 99%CPU (0avgtext+0avgdata 720444maxresident)k
0inputs+0outputs (0major+180537minor)pagefaults 0swaps

time ./perm_count gc 10
3628800
2.81user 0.19system 0:03.01elapsed 99%CPU (0avgtext+0avgdata 715520maxresident)k
0inputs+0outputs (0major+179301minor)pagefaults 0swaps

time ./perm_count global-roots 10
3628800
13.67user 0.31system 0:13.99elapsed 99%CPU (0avgtext+0avgdata 851608maxresident)k
0inputs+0outputs (0major+213372minor)pagefaults 0swaps

time ./perm_count generational-global-roots 10
3628800
5.54user 0.26system 0:05.81elapsed 99%CPU (0avgtext+0avgdata 853736maxresident)k
0inputs+0outputs (0major+213913minor)pagefaults 0swaps

time ./perm_count fake-boxroots 10
3628800
5.67user 0.31system 0:05.99elapsed 99%CPU (0avgtext+0avgdata 967444maxresident)k
0inputs+0outputs (0major+242288minor)pagefaults 0swaps

time ./perm_count fast-boxroots 10
3628800
2.70user 0.30system 0:03.00elapsed 99%CPU (0avgtext+0avgdata 762360maxresident)k
0inputs+0outputs (0major+191032minor)pagefaults 0swaps
```

We see that global roots add a large overhead (14s compared to 3s when
using the OCaml GC), which is largely reduced by using generational
global roots (6s).

"Fake" boxroots (generational roots made movable by placing them in
their owned malloc'ed value) are barely slower than generational
global roots.

"Fast" boxroots (implemented with a custom allocator) are competitive
with implementations that do not use per-element roots.
