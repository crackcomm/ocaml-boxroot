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
- `choice_ocaml`: a reference implementation in OCaml
- `choice_gc`: a reference implementation in C,
  using lists managed by the OCaml GC (no per-element root)
- `choice_global_roots`: a global root per element
- `choice_generational_roots`: a generational global root per element

## Some numbers on my machine

```
$ make bench
time ./perm_count ocaml 10
3628800
2.06user 0.23system 0:02.30elapsed 99%CPU (0avgtext+0avgdata 714660maxresident)k
0inputs+0outputs (0major+179183minor)pagefaults 0swaps

time ./perm_count gc 10
3628800
2.35user 0.20system 0:02.56elapsed 99%CPU (0avgtext+0avgdata 710388maxresident)k
0inputs+0outputs (0major+178129minor)pagefaults 0swaps

time ./perm_count global-roots 10
3628800
12.20user 0.26system 0:12.51elapsed 99%CPU (0avgtext+0avgdata 850732maxresident)k
0inputs+0outputs (0major+213209minor)pagefaults 0swaps

time ./perm_count generational-global-roots 10
3628800
4.38user 0.28system 0:04.68elapsed 99%CPU (0avgtext+0avgdata 852264maxresident)k
0inputs+0outputs (0major+213584minor)pagefaults 0swaps
```

We see that global roots add a large overhead (12s compared to 2s when
using the OCaml GC), which is largely reduced by using generational
global roots (4s).
