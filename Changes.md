Working version
===============

### General changes

- Add support for OCaml 5.0 (Multicore).
  (Guillaume Munch-Maccagnoni)

### Internal changes

- Benchmark improvements.
  (Gabriel Scherer, Guillaume Munch-Maccagnoni)

### Packaging

- Minor improvements.
  (Guillaume Munch-Maccagnoni)


ocaml-boxroot 0.2
=================

### General changes

- Thread-safety using a global lock.
  (Bruno Deferrari)

### Internal changes

- Minor simplifications and performance improvements to the allocator
  and to the benchmarks.
  (Gabriel Scherer, Guillaume Munch-Maccagnoni)

### Packaging

- Add `without-ocamlopt` feature flag for compiling the Rust crate
  without an OCaml install requirement.
  (Bruno Deferrari)


ocaml-boxroot 0.1
=================

- First numbered prototype & experimentation.
  (Bruno Deferrari, Guillaume Munch-Maccagnoni, Gabriel Scherer,
  Jacques-Henri Jourdan)

### Packaging

- First version published with the Rust crate ocaml-boxroot-sys.
  (Bruno Deferrari)
