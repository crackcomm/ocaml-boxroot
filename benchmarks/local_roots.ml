external local_fixpoint : (float -> float) -> float -> float = "local_fixpoint"
external boxroot_fixpoint : (float -> float) -> float -> float = "boxroot_fixpoint"

external boxroot_setup : unit -> unit = "caml_boxroot_setup"
external boxroot_teardown : unit -> unit = "caml_boxroot_teardown"

external boxroot_stats : unit -> unit = "caml_boxroot_stats"

let fixpoint =
  match Sys.getenv "ROOT" with
  | "local" -> local_fixpoint
  | "boxroot" -> boxroot_fixpoint
  | other ->
    Printf.eprintf "Unknown environment value ROOT=%S, expected 'local' or 'boxroot'" other;
    exit 2
  | exception _ ->
    Printf.eprintf "We expect an environment variable ROOT, defined as either 'local' or 'boxroot'";
    exit 2

let n =
  try int_of_string (Sys.getenv "N") with
  | _ ->
    Printf.eprintf "We expect an environment variable N, whose value is an integer";
    exit 2

let show_stats =
  match Sys.getenv "STATS" with
  | "true" | "1" | "yes" -> true
  | "false" | "0" | "no" -> false
  | _ | exception _ -> false

let () =
  boxroot_setup ();
  for _i = 1 to (100_000_000 / (max 1 n)) do
    ignore (fixpoint (fun x -> if truncate x >= n then x else x +. 1.) 1.)
  done;
  if show_stats then boxroot_stats ();
  boxroot_teardown ();
  Printf.printf "local_roots(ROOT=%-7s, N=%s): %.2fs\n%!"
    (Sys.getenv "ROOT") (Sys.getenv "N") (Sys.time ())
