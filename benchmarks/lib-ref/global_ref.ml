type 'a t
external create : 'a -> 'a t         = "global_ref_create"
external get : 'a t -> 'a            = "global_ref_get"
external modify : 'a t array -> int -> 'a -> unit = "global_ref_modify"
external delete : 'a t -> unit       = "global_ref_delete"

let setup () = ()
let teardown () = ()
let print_stats () = ()
