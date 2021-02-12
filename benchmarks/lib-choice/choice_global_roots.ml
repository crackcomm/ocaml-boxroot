type 'a t

external map : ('a -> 'b) -> 'a t -> 'b t = "choice_gr_map"

external return : 'a -> 'a t = "choice_gr_return"
external pair : 'a t -> 'b t -> ('a * 'b) t = "choice_gr_pair"

external bind : ('a -> 'b t) -> 'a t -> 'b t = "choice_gr_bind"

external fail : unit -> 'a t = "choice_gr_fail"
external choice : 'a t -> 'a t -> 'at = "choice_gr_choice"

external run : 'a t -> ('a -> unit) -> unit = "choice_gr_run"
