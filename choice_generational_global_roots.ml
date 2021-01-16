type 'a t

external map : ('a -> 'b) -> 'a t -> 'b t = "choice_ggr_map"

external return : 'a -> 'a t = "choice_ggr_return"
external pair : 'a t -> 'b t -> ('a * 'b) t = "choice_ggr_pair"

external bind : ('a -> 'b t) -> 'a t -> 'b t = "choice_ggr_bind"

external fail : unit -> 'a t = "choice_ggr_fail"
external choice : 'a t -> 'a t -> 'at = "choice_ggr_choice"

external run : 'a t -> ('a -> unit) -> unit = "choice_ggr_run"
