C_HEADERS=$(wildcard boxroot/*.h choice/*.h)

BOXROOT_LIB = $(addprefix boxroot/,\
  boxroot.o \
)

CHOICE_MODULES = $(addprefix choice/,\
  choice_ocaml_persistent.cmx \
  choice_ocaml_ephemeral.cmx \
  abstract_value.o \
  choice_gc_stubs.o choice_gc.cmx \
  choice_global_roots_stubs.o choice_global_roots.cmx \
  choice_generational_global_roots_stubs.o choice_generational_global_roots.cmx \
  fake_boxroot.o choice_fake_boxroots_stubs.o choice_fake_boxroots.cmx \
  choice_boxroots_stubs.o choice_boxroots.cmx \
  config.cmx \
)

BENCHMARKS = $(addprefix benchmarks/,\
  perm_count.exe \
)

LIB_DIRS=boxroot choice
LIB_DIRS_INCLUDE=$(foreach DIR,LIB_DIRS, -I $(DIR))

.PHONY: all
all: $(BENCHMARKS)

show-libs:
	@echo $(BOXROOT_LIB) $(CHOICE_MODULES)

%.exe: $(BOXROOT_LIB) $(CHOICE_MODULES) %.cmx
	ocamlopt $(INCLUDE_LIB_DIRS) -g -o $@ $^

%.cmx: %.ml
	ocamlopt $(INCLUDE_LIB_DIRS) -g -c $< -o $@

# before OCaml 4.12, (ocamlopt -c foo/bar.c -o foo/bar.o) is not supported
# (-c and -o together are rejected)
%.o: %.c $(C_HEADERS)
	$(shell ocamlopt -config-var native_c_compiler) -g -I'$(shell ocamlopt -where)' -I. -c $< -o $@

clean:
	rm -f $(foreach DIR, $(LIB_DIRS) benchmarks, $(DIR)/*.cm* $(DIR)/*.o)

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
	$(call run_bench,benchmarks/perm_count.exe,10)

.PHONY: test-boxroot
test-boxroot: benchmarks/perm_count.exe
	NITER=10 IMPLEM=boxroots ./perm_count.exe
