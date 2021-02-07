EMPTY=

CHOICE_MODULES = \
  choice_ocaml_persistent.cmx \
  choice_ocaml_ephemeral.cmx \
  choice_gc_stubs.o choice_gc.cmx \
  abstract_value.o \
  choice_global_roots_stubs.o choice_global_roots.cmx \
  choice_generational_global_roots_stubs.o choice_generational_global_roots.cmx \
  fake_boxroot.o choice_fake_boxroots_stubs.o choice_fake_boxroots.cmx \
  boxroot/boxroot.o choice_boxroots_stubs.o choice_boxroots.cmx \
  $(EMPTY)

LIBS = \
  config.cmx \
  $(EMPTY)

BENCHMARKS = \
  perm_count.exe \
  $(EMPTY)

.PHONY: all
all: $(BENCHMARKS)

%.exe: $(CHOICE_MODULES) $(LIBS) %.cmx
	ocamlopt -g -o $@ $^

%.cmx: %.ml
	ocamlopt -g -c $< -o $@

# before OCaml 4.12, (ocamlopt -c foo/bar.c -o foo/bar.o) is not supported
# (-c and -o together are rejected)
%.o: %.c *.h
	$(shell ocamlopt -config-var native_c_compiler) -g -I'$(shell ocamlopt -where)' -c $< -o $@

clean:
	rm -f $(foreach dir, . boxroot, $(dir)/*.cm* $(dir)/*.o)

EMPTY=

IMPLEMENTATIONS=\
  ocaml-persistent \
  ocaml-ephemeral \
  gc \
  global-roots \
  generational-global-roots \
  fake-boxroots \
  boxroots \
  $(EMPTY)

run_bench = \
  export NITER=$(2) $(foreach IMPLEM, $(IMPLEMENTATIONS), \
    && (echo "---"; IMPLEM=$(IMPLEM) ./$(1)) \
  )

.PHONY: bench
bench: $(BENCHMARKS)
	$(call run_bench,perm_count.exe,10)
