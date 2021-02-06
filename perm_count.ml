module Config = struct
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

  let choice =
    try List.assoc (Sys.getenv "IMPLEM") choices
    with _ -> wrong_usage ()

  let n =
    try int_of_string (Sys.getenv "NITER")
    with _ -> wrong_usage ()
end

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
  match Config.choice with
  | `OCaml_persistent -> (module Choice_ocaml_persistent)
  | `OCaml_ephemeral -> (module Choice_ocaml_ephemeral)
  | `Gc -> (module Choice_gc)
  | `Global_roots -> (module Choice_global_roots)
  | `Generational_global_roots -> (module Choice_generational_global_roots)
  | `Fake_boxroots -> (module Choice_fake_boxroots)
  | `Boxroots -> (module Choice_boxroots)

module Choice = (val choice_module : LinChoice)

open Choice
let ( let+ ) a f = map f a
let ( and+ ) a1 a2 = pair a1 a2
let ( let* ) m f = bind f m

let rec insert : type a . a -> a list -> a list Choice.t =
  fun elt xs -> match xs with
  | [] -> return [elt]
  | x :: xs ->
    choice
      (return (elt :: x :: xs))
      (let+ xs' = insert elt xs in x :: xs')

let rec permutation : type a . a list -> a list Choice.t = function
  | [] -> return []
  | x :: xs ->
    let* xs' = permutation xs in
    insert x xs'

(* (range n) is [0; ..; n-1] *)
let range n =
  let rec loop acc n =
    if n < 0 then acc
    else loop (n :: acc) (n - 1)
  in loop [] (n - 1)

let debug = false

(* the number could be large, so count it as an int64 *)
let count_permutations n =
  let counter = ref Int64.zero in
  let input = range n in
  let perm = permutation input in
  Choice.run perm
    (fun li ->
       if debug then begin
         List.iter (Printf.printf "%d ") li; print_newline ();
       end;
       counter := Int64.succ !counter);
  !counter

let () =
  let count = count_permutations Config.n in
  Printf.printf "%Ld\n%!" count;
