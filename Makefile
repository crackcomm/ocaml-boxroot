# Entry points

BENCHMARKS = $(addprefix benchmarks/,\
  perm_count.exe \
  synthetic.exe \
)

.PHONY: entry
entry:
	@echo "make all: build all benchmarks"
	@echo "make run: run all benchmarks"
	@echo "make run-perm_count: run perm_count benchmark"
	@echo "make run-synthetic: run synthetic benchmark"
	@echo "make test-boxroot: test boxroot on perm_count"
	@echo "make clean"

.PHONY: all
all: $(BENCHMARKS)
	@echo "Available benchmarks:" $(BENCHMARKS)

benchmarks/%.exe: _build/benchmarks/%.exe
	cp $< $@

EMPTY=
REF_IMPLS=\
  ocaml \
  gc \
  boxroot \
  global \
  generational \
  $(EMPTY)

run_bench = \
  echo "Benchmark: $(1)" \
  && echo "---" \
  $(foreach REF, $(REF_IMPLS), \
    && (REF=$(REF) $(2); echo "---") \
  )

.PHONY: run-perm_count
run-perm_count: $(BENCHMARKS)
	$(call run_bench,"perm_count", \
	  CHOICE=persistent N=10 ./benchmarks/perm_count.exe)

.PHONY: run-synthetic
run-synthetic: $(BENCHMARKS)
	$(call run_bench,"synthetic", \
	    N=8 \
	    CHOICE=ephemeral \
	    SMALL_ROOTS=10_000 \
	    LARGE_ROOTS=20 \
	    SMALL_ROOT_PROMOTION_RATE=0.2 \
	    LARGE_ROOT_PROMOTION_RATE=1 \
	    ROOT_SURVIVAL_RATE=0.9 \
	    GC_PROMOTION_RATE=0.1 \
	    GC_SURVIVAL_RATE=0.5 \
	    ./benchmarks/synthetic.exe \
	)

.PHONY: run
run: run-perm_count run-synthetic

.PHONY: test-boxroot
test-boxroot: benchmarks/perm_count.exe
	N=10 REF=boxroot CHOICE=ephemeral benchmarks/perm_count.exe

clean:
	rm -fR _build



# Build rules

C_HEADERS=$(addprefix _build/,$(wildcard \
   boxroot/*.h \
   benchmarks/lib-ref/*.h \
   benchmarks/lib-choice/*.h))

BOXROOT_LIB = $(addprefix _build/boxroot/,\
  boxroot.o \
)

REF_MODULES = $(addprefix _build/benchmarks/lib-ref/,\
  abstract_out_of_heap.o \
  global.o global_ref.cmx \
  generational.o generational_ref.cmx \
  gc.o gc_ref.cmx \
  boxroot.o boxroot_ref.cmx \
  ocaml_ref.cmx \
  ref_config.cmx \
)

CHOICE_MODULES = $(addprefix _build/benchmarks/lib-choice/,\
  choice_persistent.cmx \
  choice_ephemeral.cmx \
  choice_config.cmx \
)

LIB_DIRS=boxroot benchmarks/lib-ref benchmarks/lib-choice
INCLUDE_LIB_DIRS=$(foreach DIR,$(LIB_DIRS), -I _build/$(DIR))

# for debugging
.PHONY: show-deps
show-deps:
	@echo $(C_HEADERS) $(BOXROOT_LIB) $(REF_MODULES) $(CHOICE_MODULES)

%.exe: $(BOXROOT_LIB) $(REF_MODULES) $(CHOICE_MODULES) %.cmx
	@mkdir -p $(shell dirname ./$@)
	ocamlopt -runtime-variant i $(INCLUDE_LIB_DIRS) -g -o $@ $^

# see http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: %.exe %.cmx %.o
# copy rules
.PRECIOUS: _build/%.c _build/%.h _build/%.ml
_build/%.c: %.c
	@mkdir -p $(shell dirname ./$@)
	echo '#line 1 "$<"' | cat - $< > $@

_build/%.h: %.h
	@mkdir -p $(shell dirname ./$@)
	echo '#line 1 "$<"' | cat - $< > $@

_build/%.ml: %.ml
	@mkdir -p $(shell dirname ./$@)
	echo '#1 "$<"' | cat - $< > $@

# build rules
%.cmx: %.ml
	@mkdir -p $(shell dirname ./$@)
	ocamlopt $(INCLUDE_LIB_DIRS) -g -c $< -o $@

# before OCaml 4.12, (ocamlopt -c foo/bar.c -o foo/bar.o) is not supported
# (-c and -o together are rejected)
.SECONDARY: $(C_HEADERS)
%.o: %.c $(C_HEADERS)
	$(shell ocamlopt -config-var native_c_compiler) -g \
	  -I'$(shell ocamlopt -where)' -I_build -I$(shell dirname $<) \
	  -DBOXROOT_STATS \
	  -c $< -o $@
