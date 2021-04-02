# Entry points

.PHONY: entry
entry:
	@echo "make all: build all benchmarks"
	@echo "make run: run all benchmarks (important tests only)"
	@echo "make run-perm_count: run the 'perm_count' benchmark"
	@echo "make run-synthetic: run the 'synthetic' benchmark"
	@echo "make run-globroots: run the 'globroots' benchmark"
	@echo "make test-boxroot: test boxroots on 'perm_count'"
	@echo "make clean"
	@echo
	@echo "Note: for each benchmark-running target you can set TEST_MORE=1"
	@echo "to enable some less-important benchmarks that are disabled by default"
	@echo "  make run-globroots TEST_MORE=1"

.PHONY: all
all:
	dune build @all

.PHONY: clean
clean:
	dune clean

EMPTY=
REF_IMPLS=\
  gc \
  boxroot \
  $(EMPTY)
REF_IMPLS_GLOBAL=\
  ocaml \
  global \
  generational \
  $(EMPTY)

run_bench = \
  echo "Benchmark: $(1)" \
  && echo "---" \
  $(foreach REF, $(REF_IMPLS) $(if $(TEST_MORE),$(REF_IMPLS_GLOBAL),), \
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
	$(MAKE) run-globroots-all TEST_MORE=1
# Globroots has the unusual behavior that global-root implementations
# are fast, faster than boxroot. We always want to  see their results,
# so we add an indirection to set TEST_MORE=1.

.PHONY: run-globroots-all
run-globroots-all: all
	$(call run_bench,"globroots", \
	  N=500_000 dune exec ./benchmarks/globroots.exe)

.PHONY: run-local_roots
run-local_roots: all
	echo "Benchmark: local_roots" \
	&& echo "---" \
	$(foreach N, 1 2 5 10 100 1000, \
	  $(foreach ROOT, local boxroot $(if $(TEST_MORE),generational,), \
	    && (N=$(N) ROOT=$(ROOT) dune exec ./benchmarks/local_roots.exe) \
	  ) && echo "---")

.PHONY: run
run:
	$(MAKE) run-perm_count
	$(MAKE) run-synthetic
	$(MAKE) run-globroots
	$(MAKE) run-local_roots

.PHONY: run-more
run-more:
	$(MAKE) run TEST_MORE=1

.PHONY: test-boxroot
test-boxroot: all
	N=10 REF=boxroot CHOICE=ephemeral dune exec benchmarks/perm_count.exe
