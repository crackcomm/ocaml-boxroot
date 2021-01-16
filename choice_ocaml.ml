(* We want to implement a *linear* choice monad, in the sense that the
   monadic combinators "consume" ownership of their inputs (in the
   monad type). This restricts the sort of programs one can write
   (we could provide "dup" combinators), but it makes it possible to
   write C versions that handle memory in a naive way -- you can free
   your input parameters.

   In this reference OCaml implementation, we dynamically enforce this
   ownership discipline with an "alive" reference on each value, which
   is set to false when it is consumed.
*)


type 'a t = {
  alive: bool ref;
  bag: 'a list;
  (* note: bag is an *unordered* list of values *)
 }

let consume { alive; bag } =
  assert !alive;
  alive := false;
  bag

let fresh bag = { alive = ref true; bag }

(* I decided to not reuse code from the standard library,
   so that it's easy to stick to the same computational behavior
   in the C versions if we want to. *)
let rec rev_append acc = function
  | [] -> acc
  | x :: xs -> rev_append (x :: acc) xs

let rec rev_append_map f acc = function
  | [] -> acc
  | x :: xs -> rev_append_map f (f x :: acc) xs

let map f li =
  (* unordered map *)
  consume li
  |> rev_append_map f []
  |> fresh

let return x = fresh [x]

let pair l1 l2 =
  let l1, l2 = consume l1, consume l2 in
  let rec pair1 acc xs1 l2 =
    match xs1 with
    | [] -> acc
    | x1 :: xs1 -> pair2 acc x1 xs1 l2 l2
  and pair2 acc x1 xs1 xs2 l2 =
    match xs2 with
    | [] -> pair1 acc xs1 l2
    | x2::xs2 -> pair2 ((x1, x2) :: acc) x1 xs1 xs2 l2
  in
  fresh (pair1 [] l1 l2)

let bind f li =
  let rec rev_flatten_map f acc = function
    | [] -> acc
    | x :: xs ->
      let ys = consume (f x) in
      rev_flatten_map f (rev_append acc ys) xs
  in
  consume li
  |> rev_flatten_map f []
  |> fresh

let fail () = fresh []
let choice l1 l2 = fresh (rev_append (consume l1) (consume l2))

let rec run li f =
  let rec iter f = function
    | [] -> ()
    | x :: xs -> f x; iter f xs
  in iter f (consume li)
