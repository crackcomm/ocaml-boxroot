let choices = [
  "ocaml-persistent", `OCaml_persistent;
  "ocaml-ephemeral", `OCaml_ephemeral;
  "gc", `Gc;
  "global-roots", `Global_roots;
  "generational-global-roots", `Generational_global_roots;
  "fake-boxroots", `Fake_boxroots;
  "boxroots", `Boxroots;
]

let wrong_usage () =
  Printf.eprintf "Usage: IMPLEM=[%s] NITER=<int> %s\n%!"
    (String.concat "|" (List.map fst choices))
    Sys.argv.(0);
  exit 2

let implem =
  try Sys.getenv "IMPLEM"
  with _ -> wrong_usage ()

let choice =
  try List.assoc implem choices
  with _ -> wrong_usage ()

let n =
  try int_of_string (Sys.getenv "NITER")
  with _ -> wrong_usage ()

(* this *linear* function consumes the ownership of its argument *)
type ('a, 'b) linfun = 'a -> 'b
module type LinChoice = sig
  (* a choice/non-determinism monad whose combinators are linear (own their arguments);
     this is pointed out explicitly so that C implementation can free their input structures
     without having to track liveness. *)
    type 'a t

    val map : ('a -> 'b) -> ('a t, 'b t) linfun

    val return : 'a -> 'a t
    val pair : ('a t, ('b t, ('a * 'b) t) linfun) linfun

    val bind : ('a -> 'b t) -> ('a t, 'b t) linfun

    val fail : unit -> 'a t
    val choice : ('a t, ('a t, 'a t) linfun) linfun

    val run : ('a t, ('a -> unit) -> unit) linfun
end

let choice_module : (module LinChoice) =
  match choice with
  | `OCaml_persistent -> (module Choice_ocaml_persistent)
  | `OCaml_ephemeral -> (module Choice_ocaml_ephemeral)
  | `Gc -> (module Choice_gc)
  | `Global_roots -> (module Choice_global_roots)
  | `Generational_global_roots -> (module Choice_generational_global_roots)
  | `Fake_boxroots -> (module Choice_fake_boxroots)
  | `Boxroots -> (module Choice_boxroots)

module Choice = struct
  include (val choice_module : LinChoice)
  let ( let+ ) a f = map f a
  let ( and+ ) a1 a2 = pair a1 a2
  let ( let* ) m f = bind f m
end
