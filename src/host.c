/* src/host.c — the self-hosting trampoline over psda programs. Slate-only.
   SPDX-License-Identifier: MIT OR Apache-2.0

   A program is a cell: its payload is the body (a leaf the floor runs), its rest is the tail (the next
   program). emit composes one; run computes its body through the floor; tail is the hand-off. Because
   the tail is the program's own next, host is exactly the shared walk — cse_map running each program
   and following the chain. No bytes, no stack, no growing graph; the byte work stays in the floor
   provider, below the seam. */
#include "cse/host.h"
#include "cse/par/run.h"
#include "cse/pool.h"
#include "cse/dsa/cell.h"
#include "cse/dsa/walk.h"

slate_psda *cse_host_emit(slate_psda *body, slate_psda *tail) {
  return cse_cell(cse_cell_take(cse_pool()), body, tail, 0);   /* payload = body, rest = tail */
}

slate_psda *cse_host_run(slate_psda *prog) {
  return cse_par_run(cse_cell_payload(prog), cse_pool());       /* run the body through the floor */
}

slate_psda *cse_host_tail(slate_psda *prog) {
  return cse_cell_rest(prog);                                   /* the next program, or nothing */
}

/* the trampoline: run each program along the tail chain, collecting the values — the shared walk. */
slate_psda *cse_host(slate_psda *seed) {
  return cse_map(cse_host_run, seed, cse_pool());
}
