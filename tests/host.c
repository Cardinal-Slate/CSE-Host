/* tests/host.c — one program tails into another, and the trampoline drives both to results.
   SPDX-License-Identifier: MIT OR Apache-2.0 */
#include <stdio.h>
#include "cse/host.h"
#include "cse/dsa/cell.h"
#include "slate/psda.h"
#include "slate/encode.h"

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", (m)); fails++; } } while (0)

/* a program body: the fold-VM leaf "push N" — one instruction [op=PUSH, nbytes=1, N] */
static unsigned char pbuf[8][8];
static slate_psda pnodes[8];
static int pn = 0;
static slate_psda *push(unsigned n) {
  unsigned char raw[3] = { 0, 1, (unsigned char)n };
  slate_encode(raw, 3, pbuf[pn]);
  pnodes[pn].potential = pbuf[pn]; pnodes[pn].prev = 0; pnodes[pn].next = 0;
  return &pnodes[pn++];
}
static unsigned long val(slate_reading r) {
  unsigned char b[16]; size_t k = slate_decode(r, b, sizeof b), i; unsigned long v = 0;
  for (i = 0; i < k; i++) v = (v << 8) | b[i];
  return v;
}

int main(void) {
  /* emit two programs: A computes 20 and tails to B; B computes 13, no tail */
  slate_psda *B = cse_host_emit(push(13), 0);
  slate_psda *A = cse_host_emit(push(20), B);

  CHECK(cse_host_tail(A) == B, "A's tail is B");
  CHECK(cse_host_tail(B) == 0, "B has no tail");

  /* the trampoline drives A → B, collecting the values in order */
  slate_psda *out = cse_host(A);

  CHECK(out != 0, "host produced results");
  CHECK(val(cse_cell_payload(out)->potential) == 20, "program A ran → 20");
  CHECK(cse_cell_rest(out) != 0 && val(cse_cell_payload(cse_cell_rest(out))->potential) == 13,
        "the tail ran: program B → 13");

  printf(fails ? "host: FAIL\n" : "host: ok\n");
  return fails ? 1 : 0;
}
