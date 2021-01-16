CHOICE_MODULES = \
  choice_ocaml.cmx \
  choice_gc_stubs.o choice_gc.cmx \
  abstract_value.o \
  choice_global_roots_stubs.o choice_global_roots.cmx \
  choice_generational_global_roots_stubs.o choice_generational_global_roots.cmx

perm_count: $(CHOICE_MODULES) perm_count.ml
	ocamlopt -g -c perm_count.ml
	ocamlopt -g -o $@ $(CHOICE_MODULES) perm_count.cmx

%.cmx: %.ml
	ocamlopt -g -c $<

%.o: %.c *.h
	ocamlopt -g -c $<

clean:
	rm -f *.cm* *.o
