type 'a t

external map : ('a -> 'b) -> 'a t -> 'b t = "choice_br_map"

external return : 'a -> 'a t = "choice_br_return"
external pair : 'a t -> 'b t -> ('a * 'b) t = "choice_br_pair"

external bind : ('a -> 'b t) -> 'a t -> 'b t = "choice_br_bind"

external fail : unit -> 'a t = "choice_br_fail"
external choice : 'a t -> 'a t -> 'at = "choice_br_choice"

external run : 'a t -> ('a -> unit) -> unit = "choice_br_run"
