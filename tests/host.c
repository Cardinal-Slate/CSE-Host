/* tests/host.c — the driver runs a small program to a fixed point. The step is supplied here (the driver
   is generic); a program encodes (n, acc), the step folds n into acc and counts down, and the host drives
   it until n hits 0 — computing 1+2+…+5 = 15. SPDX-License-Identifier: MIT OR Apache-2.0 */
#include <stdio.h>
#include "cse/host.h"
#include "slate/psda.h"
#include "slate/encode.h"

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", (m)); fails++; } } while (0)

/* a program is a psda whose potential encodes two small non-negative bytes: (n, acc) */
static unsigned char pbuf[64][4];
static slate_psda pnodes[64];
static int pn = 0;
static slate_psda *mk(int n, int acc) {
  unsigned char raw[2] = { (unsigned char)n, (unsigned char)acc };
  slate_encode(raw, 2, pbuf[pn]);
  pnodes[pn].potential = pbuf[pn]; pnodes[pn].prev = 0; pnodes[pn].next = 0;
  return &pnodes[pn++];
}
static void get(slate_psda *p, int *n, int *acc) {
  unsigned char raw[4]; slate_decode(p->potential, raw, sizeof raw);
  *n = raw[0]; *acc = raw[1];
}

/* the step: fold n into acc, decrement n; done (return 0) when n reaches 0 */
static slate_psda *sum_step(slate_psda **pool, slate_psda *prog) {
  int n, acc; (void)pool;
  get(prog, &n, &acc);
  if (n <= 0) return 0;
  return mk(n - 1, acc + n);
}

int main(void) {
  slate_psda *pool = 0;   /* this step allocates from its own static table, so no pool is needed */

  slate_psda *result = cse_host(sum_step, &pool, mk(5, 0));
  int n, acc; get(result, &n, &acc);

  CHECK(n == 0, "host drove the program to its fixed point (n = 0)");
  CHECK(acc == 15, "host summed 1..5 = 15 by iterating the step");

  if (!fails) printf("  host: drove (5,0) → (0,15) — 1+2+3+4+5 = 15\n");
  printf(fails ? "host: FAIL\n" : "host: ok\n");
  return fails ? 1 : 0;
}
