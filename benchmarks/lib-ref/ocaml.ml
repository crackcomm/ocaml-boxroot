type 'a t = 'a

let create x = x

let get r = r

let modify a i v = a.(i) <- v

let delete _r = ()

let setup () = ()
let teardown () = ()
let print_stats () = ()
