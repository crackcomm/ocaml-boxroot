CHOICE_MODULES = \
  choice_ocaml.cmx \
  choice_gc_stubs.o choice_gc.cmx \
  abstract_value.o \
  choice_global_roots_stubs.o choice_global_roots.cmx \
  choice_generational_global_roots_stubs.o choice_generational_global_roots.cmx \
  boxroot.o choice_boxroots_stubs.o choice_boxroots.cmx

perm_count: $(CHOICE_MODULES) perm_count.ml
	ocamlopt -g -c perm_count.ml
	ocamlopt -g -o $@ $(CHOICE_MODULES) perm_count.cmx

%.cmx: %.ml
	ocamlopt -g -c $<

%.o: %.c *.h
	ocamlopt -g -c $<

clean:
	rm -f *.cm* *.o

.PHONY: bench
bench: perm_count
	time ./perm_count ocaml 10
	@echo
	time ./perm_count gc 10
	@echo
	time ./perm_count global-roots 10
	@echo
	time ./perm_count generational-global-roots 10
	@echo
	time ./perm_count fake-boxroots 10
