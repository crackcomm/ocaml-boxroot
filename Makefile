# Entry points

BENCHMARKS = $(addprefix benchmarks/,\
  perm_count.exe \
  synthetic.exe \
  globroots.exe \
)

.PHONY: entry
entry:
	@echo "make all: build all benchmarks"
	@echo "make run: run all benchmarks"
	@echo "make run-perm_count: run the 'perm_count' benchmark"
	@echo "make run-synthetic: run the 'synthetic' benchmark"
	@echo "make run-globroots: run the 'globroots' benchmark"
	@echo "make test-boxroot: test boxroots on 'perm_count'"
	@echo "make clean"

.PHONY: all
all:
	dune build @all

clean:
	dune clean

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
run-perm_count: all
	$(call run_bench,"perm_count", \
	  CHOICE=persistent N=10 dune exec ./benchmarks/perm_count.exe)

.PHONY: run-synthetic
run-synthetic: all
	$(call run_bench,"synthetic", \
	    N=8 \
	    SMALL_ROOTS=10_000 \
	    LARGE_ROOTS=20 \
	    SMALL_ROOT_PROMOTION_RATE=0.2 \
	    LARGE_ROOT_PROMOTION_RATE=1 \
	    ROOT_SURVIVAL_RATE=0.99 \
	    GC_PROMOTION_RATE=0.1 \
	    GC_SURVIVAL_RATE=0.5 \
	    dune exec ./benchmarks/synthetic.exe \
	)

.PHONY: run-globroots
run-globroots: all
	$(call run_bench,"globroots", \
	  N=500_000 dune exec ./benchmarks/globroots.exe)

.PHONY: run
run:
	$(MAKE) run-perm_count
	$(MAKE) run-synthetic
	$(MAKE) run-globroots

.PHONY: test-boxroot
test-boxroot: all
	N=10 REF=boxroot CHOICE=ephemeral dune exec benchmarks/perm_count.exe
