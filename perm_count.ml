module Choice = Config.Choice
open Choice

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
  Printf.printf "%s: %!" Config.implem;
  let count = count_permutations Config.n in
  Printf.printf "%f\n%!" (Sys.time ());
  Printf.printf "count: %Ld\n%!" count;
