#include "ocaml_hooks.h"

#include <caml/misc.h>

#if OCAML_MULTICORE
static atomic_int in_minor_collection = 0;
#else
static int in_minor_collection = 0;
#endif

static caml_timing_hook prev_minor_begin_hook = NULL;
static caml_timing_hook prev_minor_end_hook = NULL;

/* In OCaml 5.0, in_minor_collection records the number of parallel
   domains currently doing a minor collection.

   Correctness depends on:
   - The stop-the-world (STW) nature of minor collection, and the fact
     that the timing hooks are called inside the STW section.
   - The fact that setup_hooks and scanning_callback are called while
     holding a domain lock. Thus, setup_hooks is called outside of a
     minor collection (in_minor_collection starts at 0 correctly), and
     scanning_callback runs either entirely inside or entirely outside
     of a STW section.
*/

static void record_minor_begin()
{
  in_minor_collection++;
  if (prev_minor_begin_hook != NULL) prev_minor_begin_hook();
}

static void record_minor_end()
{
  in_minor_collection--;
  if (prev_minor_end_hook != NULL) prev_minor_end_hook();
}

int boxroot_private_in_minor_collection()
{
  return in_minor_collection != 0;
}


static boxroot_scanning_callback scanning_callback = NULL;

#if OCAML_MULTICORE

static scan_roots_hook prev_scan_roots_hook;

static void boxroot_scan_hook(scanning_action action, void *data,
                              caml_domain_state *dom_st)
{
  if (prev_scan_roots_hook != NULL) {
    (*prev_scan_roots_hook)(action, data, dom_st);
  }
  (*scanning_callback)(action, data);
}

void boxroot_private_setup_hooks(boxroot_scanning_callback f)
{
  scanning_callback = f;
  // save previous hooks and install ours
  prev_scan_roots_hook = atomic_exchange(&caml_scan_roots_hook,
                                         boxroot_scan_hook);
  prev_minor_begin_hook = atomic_exchange(&caml_minor_gc_begin_hook,
                                          record_minor_begin);
  prev_minor_end_hook = atomic_exchange(&caml_minor_gc_end_hook,
                                        record_minor_end);
}

#else

static void (*prev_scan_roots_hook)(scanning_action);

static void boxroot_scan_hook(scanning_action action)
{
  if (prev_scan_roots_hook != NULL) {
    (*prev_scan_roots_hook)(action);
  }
  (*scanning_callback)(action, NULL);
}

void boxroot_private_setup_hooks(boxroot_scanning_callback f)
{
  scanning_callback = f;
  // save previous hooks
  prev_scan_roots_hook = caml_scan_roots_hook;
  prev_minor_begin_hook = caml_minor_gc_begin_hook;
  prev_minor_end_hook = caml_minor_gc_end_hook;
  // install our hooks
  caml_scan_roots_hook = boxroot_scan_hook;
  caml_minor_gc_begin_hook = record_minor_begin;
  caml_minor_gc_end_hook = record_minor_end;
}

#endif // OCAML_MULTICORE
