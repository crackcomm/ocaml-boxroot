type 'a t = 'a option ref
let create v = ref (Some v)
let get r = Option.get !r
let modify r v = r := (Some v)
let delete r = r := None
