Working version
===============

### General changes

- Add support for OCaml 5.0 (multicore).
  (Guillaume Munch-Maccagnoni, review by Gabriel Scherer)

- Clarify license (MIT license).
  (Guillaume Munch-Maccagnoni)

### Internal changes

- Benchmark improvements.
  (Gabriel Scherer and Guillaume Munch-Maccagnoni)

- Per-domain caching for OCaml multicore. There is no longer a global
  lock for multicore.
  (Guillaume Munch-Maccagnoni)

### Experiments

- Simple implementation with a doubly-linked list
  (Gabriel Scherer, review by Jacques-Henri Jourdan and
   Guillaume Munch-Maccagnoni)

- Implementation using the remembered set and a per-pool young
  freelist.
  (Gabriel Scherer, following an idea of Stephen Dolan, review
   by Guillaume Munch-Maccagnoni)

- Optimizing for inlining.
  (Guillaume Munch-Maccagnoni, review by Gabriel Scherer)

### Packaging

- Minor improvements.
  (Guillaume Munch-Maccagnoni)

- Remove `without-ocamlopt` feature flag from the Rust crate and add
  `bundle-boxroot`.
  (Bruno Deferrari)

- Declare `package.links` value in Rust crate.


ocaml-boxroot 0.2
=================

### General changes

- Thread-safety using a global lock.
  (Bruno Deferrari)

### Internal changes

- Minor simplifications and performance improvements to the allocator
  and to the benchmarks.
  (Gabriel Scherer and Guillaume Munch-Maccagnoni)

### Packaging

- Add `without-ocamlopt` feature flag for compiling the Rust crate
  without an OCaml install requirement.
  (Bruno Deferrari)


ocaml-boxroot 0.1
=================

- First numbered prototype & experimentation.
  (Bruno Deferrari, Guillaume Munch-Maccagnoni, Gabriel Scherer)

### Packaging

- First version published with the Rust crate ocaml-boxroot-sys.
  (Bruno Deferrari)
