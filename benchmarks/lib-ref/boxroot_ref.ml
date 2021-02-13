type 'a t
external create : 'a -> 'a t         = "boxroot_ref_create"
external get : 'a t -> 'a            = "boxroot_ref_get"
external modify : 'a t -> 'a -> unit = "boxroot_ref_modify"
external delete : 'a t -> unit       = "boxroot_ref_delete"

external setup : unit -> unit = "boxroot_ref_setup"
external teardown : unit -> unit = "boxroot_ref_teardown"

let () =
  setup ();
  at_exit (fun () -> teardown ())
