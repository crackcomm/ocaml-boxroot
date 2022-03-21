type impl = {
  fixpoint: (float -> float) -> float -> float;
  setup: unit -> unit;
  teardown: unit -> unit;
  stats: unit -> unit;
}

external local_fixpoint : (float -> float) -> float -> float = "local_fixpoint"
external boxroot_fixpoint : (float -> float) -> float -> float = "boxroot_fixpoint"
external dll_boxroot_fixpoint : (float -> float) -> float -> float = "dll_boxroot_fixpoint"
external generational_fixpoint : (float -> float) -> float -> float = "generational_fixpoint"

external boxroot_setup : unit -> unit = "caml_boxroot_setup"
external boxroot_teardown : unit -> unit = "caml_boxroot_teardown"
external boxroot_stats : unit -> unit = "caml_boxroot_stats"

external dll_boxroot_setup : unit -> unit = "caml_dll_boxroot_setup"
external dll_boxroot_teardown : unit -> unit = "caml_dll_boxroot_teardown"
external dll_boxroot_stats : unit -> unit = "caml_dll_boxroot_stats"


let local = {
  fixpoint = local_fixpoint;
  setup = ignore;
  teardown = ignore;
  stats = ignore;
}

let generational = {
  fixpoint = generational_fixpoint;
  setup = ignore;
  teardown = ignore;
  stats = ignore;
}

let boxroot = {
  fixpoint = boxroot_fixpoint;
  setup = boxroot_setup;
  teardown = boxroot_teardown;
  stats = boxroot_stats;
}

let dll_boxroot = {
  fixpoint = dll_boxroot_fixpoint;
  setup = dll_boxroot_setup;
  teardown = dll_boxroot_teardown;
  stats = dll_boxroot_stats;
}

let implementations = [
  "local", local;
  "boxroot", boxroot;
  "dll_boxroot", dll_boxroot;
  "generational", generational;
]

let impl =
  try List.assoc (Sys.getenv "ROOT") implementations with
  | _ ->
    Printf.eprintf "We expect an environment variable ROOT with value one of [ %s ].\n%!"
      (String.concat " | " (List.map fst implementations));
    exit 2

let n =
  let fail () =
    Printf.eprintf "We expect an environment variable N, whose value \
                    is a positive integer.";
    exit 2
  in
  match int_of_string (Sys.getenv "N") with
  | n when n < 1 -> fail ()
  | n -> n
  | exception _ -> fail ()

let show_stats =
  match Sys.getenv "STATS" with
  | "true" | "1" | "yes" -> true
  | "false" | "0" | "no" -> false
  | _ | exception _ -> false

let () =
  impl.setup ();
  Printf.printf "local_roots(ROOT=%-*s, N=%n): %!"
    (List.fold_left max 0 (List.map String.length (List.map fst implementations)))
    (Sys.getenv "ROOT") n;
  let fixpoint = impl.fixpoint in
  let num_iter = 100_000_000 / n in
  for _i = 1 to num_iter do
    ignore (fixpoint (fun x -> if truncate x >= n then x else x +. 1.) 1.)
  done;
  let time_ns = (Sys.time () *. 1E9) /. (float_of_int num_iter) in
  Printf.printf "%8.2fns\n%!" time_ns;
  if show_stats then impl.stats ();
  impl.teardown ();
