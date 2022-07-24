# Boxroot for OCaml: fast movable GC roots

This library extends the OCaml foreign function interface with an
efficient and flexible GC rooting mechanism. See
<https://gitlab.com/ocaml-rust/ocaml-boxroot/>.

This crate exposes the raw functionality of the Boxroot library as
unsafe Rust functions. It is meant to be used by low-level libraries
to expose GC roots for the OCaml GC as smart pointers in Rust (see the
package `ocaml-interop`).

## Running tests

The `link-ocaml-runtime-and-dummy-program` feature needs to be enabled when running tests:

    cargo test --features "link-ocaml-runtime-and-dummy-program"
