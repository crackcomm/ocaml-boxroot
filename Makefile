# Entry points

BENCHMARKS = $(addprefix ,\
  perm_count.bench \
)

.PHONY: entry
entry:
	@echo "make all: build all benchmarks"
	@echo "make run: run all benchmarks"
	@echo "make test-boxroot: test boxroot on perm_count"
	@echo "make clean"

.PHONY: all
all: $(BENCHMARKS)

%.bench: _build/%.exe
	cp $< $@

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
    && (echo "---"; IMPLEM=$(IMPLEM) $(1)) \
  )

.PHONY: run
run: $(BENCHMARKS)
	$(call run_bench,./perm_count.bench,10)

.PHONY: test-boxroot
test-boxroot: ./perm_count.bench
	NITER=10 IMPLEM=boxroots ./perm_count.bench

clean:
	rm -fR _build



# Build rules

C_HEADERS=$(addprefix _build/,$(wildcard boxroot/*.h choice/*.h))

BOXROOT_LIB = $(addprefix _build/boxroot/,\
  boxroot.o \
)

CHOICE_MODULES = $(addprefix _build/choice/,\
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

LIB_DIRS=boxroot choice
INCLUDE_LIB_DIRS=$(foreach DIR,$(LIB_DIRS), -I _build/$(DIR))

# for debugging
.PHONY: show-deps
show-deps:
	@echo $(C_HEADERS) $(BOXROOT_LIB) $(CHOICE_MODULES)

%.exe: $(BOXROOT_LIB) $(CHOICE_MODULES) %.cmx
	@mkdir -p $(shell dirname ./$@)
	ocamlopt -runtime-variant i $(INCLUDE_LIB_DIRS) -g -o $@ $^
# see http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.SECONDARY: $(BOXROOT_LIB) $(CHOICE_MODULES)
.PRECIOUS: %.cmx

# copy rules
.PRECIOUS: _build/%.c _build/%.h _build/%.ml
_build/%.c: %.c
	@mkdir -p $(shell dirname ./$@)
	cp $< $@

_build/%.h: %.h
	@mkdir -p $(shell dirname ./$@)
	cp $< $@

_build/%.ml: %.ml
	@mkdir -p $(shell dirname ./$@)
	cp $< $@

%.cmx: %.ml
	@mkdir -p $(shell dirname ./$@)
	ocamlopt $(INCLUDE_LIB_DIRS) -g -c $< -o $@

# before OCaml 4.12, (ocamlopt -c foo/bar.c -o foo/bar.o) is not supported
# (-c and -o together are rejected)
.SECONDARY: $(C_HEADERS)
%.o: %.c $(C_HEADERS)
	$(shell ocamlopt -config-var native_c_compiler) -g -I'$(shell ocamlopt -where)' -I_build -I$(shell dirname $<) -c $< -o $@
