# ocaml-boxroot-sys

Thin wrapper around [ocaml-boxroot](https://gitlab.com/ocaml-rust/ocaml-boxroot/)

## Running tests

The `link-ocaml-runtime` feature needs to be enabled when running tests:

    cargo test --features "link-ocaml-runtime"
