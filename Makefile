CHOICE_MODULES = \
  choice_ocaml.cmx

perm_count: $(CHOICE_MODULES) perm_count.ml
	ocamlopt -g -c perm_count.ml
	ocamlopt -g -o $@ $(CHOICE_MODULES) perm_count.cmx

%.cmx: %.ml
	ocamlopt -g -c $<


%.o: %.c
	ocamlopt -g -c $<

clean:
	rm -f *.cm* *.o
