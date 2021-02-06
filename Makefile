CHOICE_MODULES = \
  choice_ocaml_persistent.cmx \
  choice_ocaml_ephemeral.cmx \
  choice_gc_stubs.o choice_gc.cmx \
  abstract_value.o \
  choice_global_roots_stubs.o choice_global_roots.cmx \
  choice_generational_global_roots_stubs.o choice_generational_global_roots.cmx \
  fake_boxroot.o choice_fake_boxroots_stubs.o choice_fake_boxroots.cmx \
  boxroot/boxroot.o choice_boxroots_stubs.o choice_boxroots.cmx

perm_count: $(CHOICE_MODULES) perm_count.ml
	ocamlopt -g -c perm_count.ml
	ocamlopt -g -o $@ $(CHOICE_MODULES) perm_count.cmx

include Makefile.common

boxroot/boxroot.o: boxroot/boxroot.c
	$(MAKE) -C boxroot boxroot.o

clean::
	make -C boxroot clean

.PHONY: bench
bench: perm_count
	time ./perm_count ocaml-persistent 10
	@echo
	time ./perm_count ocaml-ephemeral 10
	@echo
	time ./perm_count gc 10
	@echo
	time ./perm_count global-roots 10
	@echo
	time ./perm_count generational-global-roots 10
	@echo
	time ./perm_count fake-boxroots 10
	@echo
	time ./perm_count boxroots 10
