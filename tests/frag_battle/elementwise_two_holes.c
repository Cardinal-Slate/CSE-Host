/* elementwise_two_holes: battle-test the .slate fragment ABI on two holes, hole order, a hole used more
 * than once, and that splice wires arg_carriers by slot.
 *
 * Target 1: root[i] = A[i] + B[i] - A[i]   (holes A,B; A referenced twice). Exact value == B[i].
 *   - iface reports exactly 2 holes, in slot order 0,1.
 *   - arg_carriers maps by slot: pass a distinct carrier for each slot and check the wiring by making A
 *     and B numerically distinct, so a mis-wire (A<->B swap) gives a wrong answer (A+B-A collapses to B
 *     only if slot0 is A used twice and slot1 is B).
 *   - Cross-check: splice with A,B swapped at the carrier level and confirm the result tracks the carrier
 *     bound to slot0 (proving splice honors arg order, not registration order).
 *
 * Target 2: root[i] = A[i]*B[i] + A[i]   (A twice again, under mul+add). Exact value == A[i]*(B[i]+1).
 *
 * Every numeric result is checked against a hand-computed value.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"
#include "../memstore.h"

/* ---- memory sink + cursor source (same pattern as tests/unit/engine/frag_cabi.c) ---- */

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
                              else { printf("ok: %s\n", msg); } } while (0)

#define N 5

/* Build root[i] = A[i] + B[i] - A[i], holes = {A,B} in that declared order. Returns serialized bytes.
 * cidA is registered first then cidB, and holes[] lists {cidA, cidB} so slot0=A, slot1=B. */
static MsProg build_add_sub(void) {
  SlateDag *b = ms_dag();
  int64_t ph[N] = {0};
  uint32_t cidA = slate_dag_carrier(b, ph, N);
  uint32_t cidB = slate_dag_carrier(b, ph, N);
  int32_t p  = slate_dag_param(b, 0);
  int32_t lA = slate_dag_load(b, cidA, p);           /* A used ... */
  int32_t lB = slate_dag_load(b, cidB, p);
  int32_t lA2 = slate_dag_load(b, cidA, p);          /* ... twice */
  int32_t sum = slate_dag_add(b, lA, lB);            /* A + B */
  int32_t root = slate_dag_sub(b, sum, lA2);         /* (A + B) - A */
  MsProg out = {0};
  uint32_t holes[2] = {cidA, cidB};                  /* slot0 = A, slot1 = B */
  const char *rc = ms_keep(b, root, holes, 2, &out);
  CHECK(rc == NULL, "T1 save_fragment(A+B-A) ok");
  slate_dag_free(b);
  return out;
}

/* Build root[i] = A[i]*B[i] + A[i]. holes {A,B}. */
static MsProg build_mul_add(void) {
  SlateDag *b = ms_dag();
  int64_t ph[N] = {0};
  uint32_t cidA = slate_dag_carrier(b, ph, N);
  uint32_t cidB = slate_dag_carrier(b, ph, N);
  int32_t p  = slate_dag_param(b, 0);
  int32_t lA = slate_dag_load(b, cidA, p);
  int32_t lB = slate_dag_load(b, cidB, p);
  int32_t lA2 = slate_dag_load(b, cidA, p);
  int32_t prod = slate_dag_mul(b, lA, lB);           /* A*B */
  int32_t root = slate_dag_add(b, prod, lA2);        /* A*B + A */
  MsProg out = {0};
  uint32_t holes[2] = {cidA, cidB};
  const char *rc = ms_keep(b, root, holes, 2, &out);
  CHECK(rc == NULL, "T2 save_fragment(A*B+A) ok");
  slate_dag_free(b);
  return out;
}

/* splice frag over (arg0,arg1) supplied in slot order, run grid[N], read N cells. rc 0 ok. */
static int splice_run2(const MsProg *frag, const int64_t arg0[N], const int64_t arg1[N], int64_t out[N]) {
  SlateFrag *f = ms_load(frag);
  if (!f) return -1000;
  SlateDag *b = slate_dag_new();
  uint32_t c0 = slate_dag_carrier(b, arg0, N);
  uint32_t c1 = slate_dag_carrier(b, arg1, N);
  uint32_t args[2] = {c0, c1};                        /* args[slot] */
  int32_t root = -1; slate_dag_splice(b, f, args, 2, &root);
  int rc = -2000;
  if (root >= 0) {
    int64_t dims[1] = {N};
    SlateArray *a = slate_dag_run(b, root, dims, 1);
    if (a) {
      int64_t num[N], den[N];
      if (slate_array_i64_unsafe(a, num, den) == NULL) {
        int okden = 1;
        for (int i = 0; i < N; i++) { out[i] = num[i]; if (den[i] != 1) okden = 0; }
        rc = okden ? 0 : -3000;
      }
      slate_array_free(a);
    } else rc = -2500;
  } else rc = root;
  slate_dag_free(b);
  slate_frag_free(f);
  return rc;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);

  /* deliberately distinct data so a hole mis-wire changes the answer */
  int64_t A[N] = {  1,   2,   3,   4,   5};
  int64_t B[N] = {100, 200, 300, 400, 500};

  /* ===== Target 1: A + B - A  == B ===== */
  MsProg f1 = build_add_sub();
  CHECK(f1.wn > 0, "T1 fragment named by a word");

  /* iface: exactly 2 holes, slots 0 and 1 in order, both array-kind */
  {
    SlateFrag *f = ms_load(&f1);
    CHECK(f != NULL, "T1 frag_load ok");
    uint32_t np = 0, nh = 0;
    slate_iface_receipt root_r;
    const char *rc = slate_frag_iface(f, &np, NULL, &nh, &root_r);
    CHECK(rc == NULL, "T1 iface ok");
    CHECK(np == 1, "T1 nparams == 1");
    CHECK(nh == 2, "T1 nholes == 2");
    slate_hole holes[2];
    uint32_t cap = 2;
    rc = slate_frag_iface(f, NULL, holes, &cap, NULL);
    CHECK(rc == NULL, "T1 iface fill ok");
    /* order: holes must be reported slot 0 then slot 1 */
    CHECK(holes[0].slot == 0, "T1 hole[0].slot == 0");
    CHECK(holes[1].slot == 1, "T1 hole[1].slot == 1");
    CHECK(strcmp(holes[0].receipt.kind, "array") == 0, "T1 hole0 is array-kind");
    CHECK(strcmp(holes[1].receipt.kind, "array") == 0, "T1 hole1 is array-kind");
    slate_frag_free(f);
  }

  /* run with args = {A, B} in slot order -> expect exactly B (A cancels) */
  {
    int64_t out[N] = {0};
    int rc = splice_run2(&f1, A, B, out);
    CHECK(rc == 0, "T1 splice_run(A,B) ok");
    int match = 1;
    for (int i = 0; i < N; i++) if (out[i] != B[i]) match = 0;   /* hand value: A+B-A = B */
    CHECK(match, "T1 A+B-A == B");
    if (!match) { printf("   got:"); for (int i=0;i<N;i++) printf(" %lld",(long long)out[i]); printf("\n"); }
  }

  /* Slot-wiring proof: swap the carriers passed. Now slot0=B, slot1=A, so the graph computes
   * (slot0)+(slot1)-(slot0) = B + A - B = A. If splice wired by registration order instead of arg
   * order this would still yield B, so a correct A here proves arg-slot wiring. */
  {
    int64_t out[N] = {0};
    int rc = splice_run2(&f1, B, A, out);              /* arg0=B, arg1=A */
    CHECK(rc == 0, "T1 splice_run(B,A) ok");
    int match = 1;
    for (int i = 0; i < N; i++) if (out[i] != A[i]) match = 0;   /* hand value: B+A-B = A */
    CHECK(match, "T1 swapped args -> result tracks slot0 (== A)");
    if (!match) { printf("   got:"); for (int i=0;i<N;i++) printf(" %lld",(long long)out[i]); printf("\n"); }
  }

  ms_prog_free(&f1);

  /* ===== Target 2: A*B + A == A*(B+1) ===== */
  MsProg f2 = build_mul_add();
  CHECK(f2.wn > 0, "T2 fragment named by a word");
  {
    int64_t out[N] = {0};
    int rc = splice_run2(&f2, A, B, out);
    CHECK(rc == 0, "T2 splice_run(A,B) ok");
    int match = 1;
    for (int i = 0; i < N; i++) {
      int64_t want = A[i] * (B[i] + 1);               /* A*B + A = A*(B+1) */
      if (out[i] != want) match = 0;
    }
    CHECK(match, "T2 A*B+A == A*(B+1)");
    if (!match) { printf("   got:"); for (int i=0;i<N;i++) printf(" %lld",(long long)out[i]); printf("\n"); }
    /* spot-check one hand value explicitly: A[2]=3,B[2]=300 -> 3*300+3 = 903 */
    CHECK(out[2] == 903, "T2 cell[2] == 903 (3*300+3)");
  }

  /* T2 swap proof: args {B,A} computes B*A + B = A*B + B, different from A*B+A when A!=B */
  {
    int64_t out[N] = {0};
    int rc = splice_run2(&f2, B, A, out);              /* arg0=B, arg1=A */
    CHECK(rc == 0, "T2 splice_run(B,A) ok");
    int match = 1;
    for (int i = 0; i < N; i++) {
      int64_t want = B[i] * A[i] + B[i];               /* slot0*slot1 + slot0 with slot0=B */
      if (out[i] != want) match = 0;
    }
    CHECK(match, "T2 swapped -> B*A+B (slot0 doubled is B)");
    /* and it must differ from the unswapped answer, else the swap proved nothing */
    CHECK((B[0]*A[0]+B[0]) != (A[0]*B[0]+A[0]), "T2 swap is observably different");
  }

  ms_prog_free(&f2);

  if (fails == 0) printf("PASS elementwise_two_holes: all checks\n");
  else printf("FAILED: %d check(s)\n", fails);
  return fails ? 1 : 0;
}
