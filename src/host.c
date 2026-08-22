/* src/host.c — the trampoline: iterate the step to a fixed point. The whole driver. It names nothing it
   drives; a program is a psda, a step is a psda→psda, and the loop is a slate loop — self-hosting.
   SPDX-License-Identifier: MIT OR Apache-2.0 */
#include "cse/host.h"

slate_psda *cse_host(cse_step step, slate_psda **pool, slate_psda *seed) {
  slate_psda *prog = seed, *next;
  while ((next = step(pool, prog)) != 0) prog = next;   /* step until it says done */
  return prog;
}
