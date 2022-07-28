type 'a t = 'a

let create x = x

let get r = r

let modify a i v = a.(i) <- v

let delete _r = ()

external setup : unit -> unit = "boxroot_ref_setup"
external teardown : unit -> unit = "boxroot_ref_teardown"
external print_stats : unit -> unit = "boxroot_stats"
