type 'a t

external map : ('a -> 'b) -> 'a t -> 'b t = "choice_fbr_map"

external return : 'a -> 'a t = "choice_fbr_return"
external pair : 'a t -> 'b t -> ('a * 'b) t = "choice_fbr_pair"

external bind : ('a -> 'b t) -> 'a t -> 'b t = "choice_fbr_bind"

external fail : unit -> 'a t = "choice_fbr_fail"
external choice : 'a t -> 'a t -> 'at = "choice_fbr_choice"

external run : 'a t -> ('a -> unit) -> unit = "choice_fbr_run"
