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
time ./perm_count ocaml-persistent 10
3628800
2.82user 0.26system 0:03.13elapsed 98%CPU (0avgtext+0avgdata 714672maxresident)k
0inputs+0outputs (0major+179187minor)pagefaults 0swaps

time ./perm_count ocaml-ephemeral 10
3628800
2.53user 0.23system 0:02.79elapsed 98%CPU (0avgtext+0avgdata 715244maxresident)k
0inputs+0outputs (0major+179329minor)pagefaults 0swaps

time ./perm_count gc 10
3628800
2.81user 0.20system 0:03.06elapsed 98%CPU (0avgtext+0avgdata 710476maxresident)k
0inputs+0outputs (0major+178136minor)pagefaults 0swaps

time ./perm_count global-roots 10
3628800
14.81user 0.29system 0:15.23elapsed 99%CPU (0avgtext+0avgdata 850844maxresident)k
0inputs+0outputs (0major+213214minor)pagefaults 0swaps

time ./perm_count generational-global-roots 10
3628800
5.02user 0.26system 0:05.31elapsed 99%CPU (0avgtext+0avgdata 852080maxresident)k
0inputs+0outputs (0major+213582minor)pagefaults 0swaps

time ./perm_count fake-boxroots 10
3628800
5.16user 0.33system 0:05.55elapsed 99%CPU (0avgtext+0avgdata 966152maxresident)k
0inputs+0outputs (0major+242107minor)pagefaults 0swaps
```

We see that global roots add a large overhead (12s compared to 2s when
using the OCaml GC), which is largely reduced by using generational
global roots (4s).
