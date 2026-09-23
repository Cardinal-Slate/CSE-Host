/* affine_baked_mix: battle-test the .slate fragment baked-constant door.
 *
 * Target: root[i] = A[i]*3 + offset[i], with A a hole and the offset table a baked constant.
 * Focus:
 *   (1) the fragment carries the baked offset data across serialization: splice with a different A
 *       and confirm the offset is still applied, never re-supplied;
 *   (2) baked data survives a full save -> load -> splice -> run round trip, twice, with two
 *       different A arrays from one loaded fragment;
 *   (3) a rational (RNS) carrier cannot be baked: save_fragment refuses with EARGS when a Q carrier
 *       read by the root is not declared a hole, and accepts the same Q carrier declared as a hole
 *       (isolates "refused because RNS-baked" from other causes).
 *
 * Every numeric result is checked against a hand-computed value.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"
#include "../memstore.h"

/* ---- growable memory sink + cursor source over the same bytes ---- */

static int fails = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
  else { printf("PASS: %s\n", msg); } } while (0)

#define N 6
static const int64_t GAIN = 3;
static const int64_t OFFSET[N] = {100, 100, 100, 200, 200, 200}; /* the baked constant table */

/* Build and save root[i] = A[i]*3 + offset[i]. A is the hole; the offset table (not listed) bakes in. */
static MsProg build_and_save(void) {
  SlateDag *b = ms_dag();
  int64_t placeholderA[N] = {0};                       /* a hole still needs data to build/typecheck once */
  uint32_t cidA   = slate_dag_carrier(b, placeholderA, N);
  uint32_t cidOff = slate_dag_carrier(b, OFFSET, N);   /* baked into the fragment */
  int32_t p = slate_dag_param(b, 0);
  int32_t scaled = slate_dag_mul(b, slate_dag_load(b, cidA, p), slate_dag_lit(b, GAIN));
  int32_t root   = slate_dag_add(b, scaled, slate_dag_load(b, cidOff, p));
  MsProg out = {0};
  uint32_t holes[1] = {cidA};                          /* only A is a hole; the offset table bakes */
  const char *rc = ms_keep(b, root, holes, 1, &out);
  CHECK(rc == NULL, "save_fragment (A hole, OFFSET baked) returns OK");
  slate_dag_free(b);
  return out;
}

/* splice over A, run on grid[N], read the N int64 cells into out[N]. Returns 0 ok, negative on refusal. */
static int splice_run(const MsProg *frag, const int64_t A[N], int64_t out[N]) {
  SlateFrag *f = ms_load(frag);
  if (!f) return -1000;
  SlateDag *b = slate_dag_new();
  uint32_t cidA = slate_dag_carrier(b, A, N);          /* only A supplied; the offset must come from the bake */
  uint32_t args[1] = {cidA};
  int32_t root = -1; slate_dag_splice(b, f, args, 1, &root);
  int rc = -2000;
  if (root >= 0) {
    int64_t dims[1] = {N};
    SlateArray *a = slate_dag_run(b, root, dims, 1);
    if (a) {
      int64_t num[N], den[N];
      if (slate_array_i64_unsafe(a, num, den) == NULL) {
        int ok = 1;
        for (int i = 0; i < N; i++) { if (den[i] != 1) ok = 0; out[i] = num[i]; }
        rc = ok ? 0 : -3000;
      }
      slate_array_free(a);
    }
  } else rc = root;
  slate_dag_free(b);
  slate_frag_free(f);
  return rc;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);

  /* 1. build + save; the fragment must be nonempty and carry the baked offset table. */
  MsProg frag = build_and_save();
  CHECK(frag.wn > 0, "fragment named by a word");

  /* 1b. iface: exactly 1 hole (A), 1 param; the offset is not a hole (it baked in). */
  {
    SlateFrag *f = ms_load(&frag);
    CHECK(f != NULL, "frag_load ok");
    if (f) {
      uint32_t np = 0, nh = 0;
      const char *rc = slate_frag_iface(f, &np, NULL, &nh, NULL);
      CHECK(rc == NULL, "iface ok");
      CHECK(np == 1, "iface nparams == 1");
      CHECK(nh == 1, "iface nholes == 1 (only A; OFFSET baked, not a hole)");
      slate_frag_free(f);
    }
  }

  /* 2. splice+run with A different from the build placeholder; the offset is never re-supplied,
   *    so any nonzero offset in the answer proves the baked table survived. */
  {
    int64_t A1[N] = {1, 2, 3, 4, 5, 6}, out1[N] = {0};
    int rc = splice_run(&frag, A1, out1);
    CHECK(rc == 0, "splice_run #1 ok");
    /* hand: A*3 + offset = {3+100,6+100,9+100,12+200,15+200,18+200} = {103,106,109,212,215,218} */
    CHECK(out1[0] == 103 && out1[1] == 106 && out1[2] == 109 &&
          out1[3] == 212 && out1[4] == 215 && out1[5] == 218,
          "splice_run #1 values == hand-computed (baked OFFSET applied)");

    /* 3. same loaded-from-bytes fragment, a second different A (with negatives/zero). */
    int64_t A2[N] = {10, -1, 0, 7, -100, 2}, out2[N] = {0};
    rc = splice_run(&frag, A2, out2);
    CHECK(rc == 0, "splice_run #2 ok");
    /* hand: {30+100, -3+100, 0+100, 21+200, -300+200, 6+200} = {130,97,100,221,-100,206} */
    CHECK(out2[0] == 130 && out2[1] == 97 && out2[2] == 100 &&
          out2[3] == 221 && out2[4] == -100 && out2[5] == 206,
          "splice_run #2 values == hand-computed (same baked OFFSET, different A)");

    /* 3b. cross-check: the two runs share the same baked offset, so out2[i]-out1[i] == 3*(A2[i]-A1[i]). */
    int64_t A1v[N] = {1,2,3,4,5,6}, A2v[N] = {10,-1,0,7,-100,2};
    int diffok = 1;
    for (int i = 0; i < N; i++) if (out2[i] - out1[i] != 3 * (A2v[i] - A1v[i])) diffok = 0;
    CHECK(diffok, "out2-out1 == 3*(A2-A1): OFFSET identical across both splices (truly baked)");
  }

  /* 4. baked-refusal for a wide carrier: a rational (RNS/Q) carrier cannot be baked.
   *    Build root[i] = Qc[i] (a Q carrier) and save with no holes, forcing a bake of Qc -> EARGS. */
  {
    SlateDag *b = ms_dag();
    int64_t nums[N] = {1, 2, 3, 4, 5, 6};
    uint32_t cidQ = slate_dag_carrier_q(b, nums, N, /*den=*/2, /*hbits=*/64);  /* cell i = nums[i]/2 */
    CHECK(cidQ != UINT32_MAX, "carrier_q registered ok");
    int32_t p = slate_dag_param(b, 0);
    int32_t root = slate_dag_load(b, cidQ, p);
    MsProg out = {0};
    /* nholes = 0: Qc is read by root but not a hole, so save must try to bake it. */
    const char *rc = ms_keep(b, root, /*holes=*/NULL, 0, &out);
    CHECK(slate_refused(rc, "args"), "save_fragment refuses to BAKE a rational carrier with EARGS");
    CHECK(out.wn == 0, "no name handed back on the refused bake");
    ms_prog_free(&out);
    slate_dag_free(b);
  }

  /* 4b. positive control: the same rational carrier declared as a hole is accepted, proving the
   *     refusal in (4) is specifically "RNS-baked", not "RNS carrier unusable in a fragment". */
  {
    SlateDag *b = ms_dag();
    int64_t nums[N] = {1, 2, 3, 4, 5, 6};
    uint32_t cidQ = slate_dag_carrier_q(b, nums, N, 2, 64);
    int32_t p = slate_dag_param(b, 0);
    int32_t root = slate_dag_load(b, cidQ, p);
    MsProg out = {0};
    uint32_t holes[1] = {cidQ};                          /* Qc as a hole, not baked */
    const char *rc = ms_keep(b, root, holes, 1, &out);
    CHECK(rc == NULL, "save_fragment ACCEPTS a rational carrier as a HOLE (Q hole ok)");
    /* the hole's receipt should read Q. */
    if (rc == NULL && out.wn > 0) {
      SlateFrag *f = ms_load(&out);
      CHECK(f != NULL, "Q-hole fragment loads back");
      if (f) {
        uint32_t np = 0, nh = 0; slate_hole hole; uint32_t cap = 1;
        slate_frag_iface(f, &np, NULL, &nh, NULL);
        CHECK(nh == 1, "Q-hole fragment reports 1 hole");
        slate_frag_iface(f, NULL, &hole, &cap, NULL);
        CHECK(strcmp(hole.receipt.domain, "Q") == 0, "the hole's declared domain is Q (rational)");
        slate_frag_free(f);
      }
    }
    ms_prog_free(&out);
    slate_dag_free(b);
  }

  ms_prog_free(&frag);
  if (fails == 0) printf("\nALL PASS affine_baked_mix\n");
  else printf("\n%d FAIL(s) affine_baked_mix\n", fails);
  return fails ? 1 : 0;
}
