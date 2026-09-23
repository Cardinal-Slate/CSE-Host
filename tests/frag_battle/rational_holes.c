/* rational_holes: battle-test the .slate fragment ABI's PSDA domain typing for rational (Q) holes.
 *
 * 1. Build a fragment that reads a Q carrier (slate_dag_carrier_q) and sums three cells:
 *       root = A[0] + A[1] + A[2]   over grid dims={1}, one scalar cell.
 *    The hole A is a Q array. iface must report the hole domain == Q (the root iface domain is derive).
 * 2. Splice a real Q carrier and run; read the exact 9/4 via slate_array_records (bignum record path).
 * 3. Typecheck direction:
 *    - a plain int (Z) carrier spliced into that Q hole is allowed (Z ⊂ Q); result is an exact integer.
 *    - a Q carrier spliced into a Z hole must refuse with "refused" (Q ⊄ Z).
 *
 * Every numeric result is checked against a hand value. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"
#include "../memstore.h"

/* ---- a growable memory sink and a cursor source over the same bytes ---- */

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL rational_holes: %s\n", msg); fails++; } \
                              else { printf("ok: %s\n", msg); } } while (0)

/* Decode cell 0 of a result via the bignum record path into signed num/den (assumes it fits int64).
 * Returns 0 on success, negative on failure. */
static int record_cell0(SlateArray *a, int64_t *num_out, int64_t *den_out) {
  uint64_t stride = 0;
  uint64_t probe = 0;                                       /* records needs a non-NULL out even to probe */
  const char *rc = slate_array_records(a, &probe, 0, &stride);      /* size-then-fill: 0 bytes -> EOUTSIZE + stride */
  if (!slate_refused(rc, "outsize") || stride == 0) { printf("  (records probe rc=%s stride=%llu)\n", rc ? rc : "NULL", (unsigned long long)stride); return -1; }
  uint64_t n = slate_array_size(a);
  uint64_t bytes = n * stride * 8;
  uint64_t *buf = (uint64_t *)calloc((size_t)(n * stride), sizeof(uint64_t));
  rc = slate_array_records(a, buf, bytes, &stride);
  if (rc != NULL) { printf("  (records fill rc=%s)\n", rc); free(buf); return -2; }
  /* record layout: [valid, sign, L, |num| LE (L limbs), den LE (L limbs)] */
  uint64_t valid = buf[0], sign = buf[1], L = buf[2];
  if (!valid) { free(buf); return -3; }
  /* only the low limb should be nonzero for these small values; assert the rest are 0 */
  int64_t num = (int64_t)buf[3];
  int64_t den = (int64_t)buf[3 + L];
  for (uint64_t i = 1; i < L; i++) { if (buf[3 + i] != 0 || buf[3 + L + i] != 0) { free(buf); return -4; } }
  *num_out = sign ? -num : num;
  *den_out = den;
  free(buf);
  return 0;
}

/* Build the Q-hole sum fragment: root = A[0]+A[1]+A[2], A a Q hole. Returns serialized bytes. */
static MsProg build_q_sum_fragment(void) {
  SlateDag *b = ms_dag();
  /* placeholder Q data so the hole builds+typechecks once: 1/4, 3/4, 5/4 (den shared = 4) */
  int64_t nums[3] = {1, 3, 5};
  uint32_t cidA = slate_dag_carrier_q(b, nums, 3, /*den=*/4, /*hbits=*/64);
  if (cidA == 0xFFFFFFFFu) { printf("FAIL rational_holes: carrier_q rejected valid Q carrier\n"); fails++; }
  int32_t s = slate_dag_add(b,
                slate_dag_add(b, slate_dag_load(b, cidA, slate_dag_lit(b, 0)),
                                 slate_dag_load(b, cidA, slate_dag_lit(b, 1))),
                slate_dag_load(b, cidA, slate_dag_lit(b, 2)));
  MsProg out = {0};
  uint32_t holes[1] = {cidA};
  const char *rc = ms_keep(b, s, holes, 1, &out);
  CHECK(rc == NULL, "save Q-sum fragment");
  slate_dag_free(b);
  return out;
}

/* Build a Z-hole fragment: root = A[0], A a plain int (Z) hole. Returns serialized bytes. */
static MsProg build_z_fragment(void) {
  SlateDag *b = ms_dag();
  int64_t placeholder[3] = {0, 0, 0};
  uint32_t cid = slate_dag_carrier(b, placeholder, 3);
  int32_t root = slate_dag_load(b, cid, slate_dag_lit(b, 0));
  MsProg out = {0};
  uint32_t holes[1] = {cid};
  const char *rc = ms_keep(b, root, holes, 1, &out);
  CHECK(rc == NULL, "save Z hole fragment");
  slate_dag_free(b);
  return out;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);

  MsProg qfrag = build_q_sum_fragment();
  CHECK(qfrag.wn > 0, "Q fragment named by a word");

  /* ---- iface: 1 hole, hole domain must be Q; the root iface domain is derive ---- */
  {
    SlateFrag *f = ms_load(&qfrag);
    CHECK(f != NULL, "load Q fragment ok");
    if (f) {
      uint32_t np = 0, nh = 0;
      slate_iface_receipt root_r;
      const char *rc = slate_frag_iface(f, &np, NULL, &nh, &root_r);
      CHECK(rc == NULL, "iface Q fragment ok");
      CHECK(nh == 1, "iface reports 1 hole");
      slate_hole holes[1];
      uint32_t cap = 1;
      rc = slate_frag_iface(f, NULL, holes, &cap, NULL);
      CHECK(rc == NULL, "iface fill holes ok");
      CHECK(strcmp(holes[0].receipt.kind, "array") == 0, "iface hole is an array");
      CHECK(strcmp(holes[0].receipt.domain, "Q") == 0, "iface HOLE domain == Q");
      CHECK(root_r.domain == NULL /* derive/any: save_fragment serializes the root receipt domain as derive */,
            "iface ROOT domain is derive");
      slate_frag_free(f);
    }
  }

  /* ---- Splice a real Q carrier + run: sum(1/4,3/4,5/4) = 9/4 ---- */
  {
    SlateFrag *f = ms_load(&qfrag);
    SlateDag *b = slate_dag_new();
    int64_t nums[3] = {1, 3, 5};
    uint32_t cidA = slate_dag_carrier_q(b, nums, 3, 4, 64);
    uint32_t args[1] = {cidA};
    int32_t root = -1; slate_dag_splice(b, f, args, 1, &root);
    CHECK(root >= 0, "splice real Q carrier ok");
    if (root >= 0) {
      int64_t dims[1] = {1};
      SlateArray *a = slate_dag_run(b, root, dims, 1);
      CHECK(a != NULL, "run Q sum ok");
      if (a) {
        const slate_entry *re; int32_t rn; slate_array_receipt(a, &re, &rn);
        CHECK(slate_entry_is(re, rn, "verdict", "exact"), "Q sum reading exact");
        CHECK(slate_entry_is(re, rn, "domain", "Q"), "Q sum result domain == Q");
        int64_t num = 0, den = 0;
        int rc = record_cell0(a, &num, &den);
        CHECK(rc == 0, "records decode Q cell ok");
        /* 1/4 + 3/4 + 5/4 = 9/4 (canonical: gcd(9,4)=1) */
        CHECK(num == 9 && den == 4, "Q sum == 9/4");
        slate_array_free(a);
      }
    }
    slate_dag_free(b);
    slate_frag_free(f);
  }

  /* ---- Typecheck (allowed): a plain Z carrier into the Q hole (Z ⊂ Q) ---- */
  {
    SlateFrag *f = ms_load(&qfrag);
    SlateDag *b = slate_dag_new();
    int64_t zvals[3] = {2, 4, 6};                 /* plain integers */
    uint32_t cidZ = slate_dag_carrier(b, zvals, 3);
    uint32_t args[1] = {cidZ};
    int32_t root = -1; slate_dag_splice(b, f, args, 1, &root);
    CHECK(root >= 0, "splice Z carrier into Q hole ALLOWED (Z sub Q)");
    if (root >= 0) {
      int64_t dims[1] = {1};
      SlateArray *a = slate_dag_run(b, root, dims, 1);
      CHECK(a != NULL, "run Z-in-Q-hole ok");
      if (a) {
        int64_t num = 0, den = 0;
        int rc = record_cell0(a, &num, &den);
        CHECK(rc == 0, "records decode Z-in-Q cell ok");
        /* 2 + 4 + 6 = 12/1 */
        CHECK(num == 12 && den == 1, "Z-in-Q-hole sum == 12/1");
        slate_array_free(a);
      }
    }
    slate_dag_free(b);
    slate_frag_free(f);
  }

  /* ---- Typecheck (refused): a Q carrier into a Z hole must refuse with -EREFUSED (Q not sub Z) ---- */
  {
    MsProg zfrag = build_z_fragment();
    /* sanity: the Z fragment's hole domain is really Z */
    { SlateFrag *zf = ms_load(&zfrag);
      if (zf) { slate_hole hp[1]; uint32_t cap = 1;
        if (slate_frag_iface(zf, NULL, hp, &cap, NULL) == NULL)
          CHECK(strcmp(hp[0].receipt.domain, "Z") == 0, "Z fragment hole domain == Z");
        slate_frag_free(zf); } }

    SlateFrag *f = ms_load(&zfrag);
    SlateDag *b = slate_dag_new();
    int64_t nums[3] = {1, 3, 5};
    uint32_t cidQ = slate_dag_carrier_q(b, nums, 3, 4, 64);   /* a real Q carrier */
    uint32_t args[1] = {cidQ};
    const char *rc = slate_dag_splice(b, f, args, 1, NULL);
    CHECK(slate_refused(rc, "refused"), "splice Q into Z hole REFUSED with \"refused\"");
    if (!slate_refused(rc, "refused"))
      printf("  (got splice rc=%s, expected \"refused\")\n", rc ? rc : "NULL");
    slate_dag_free(b);
    slate_frag_free(f);
    ms_prog_free(&zfrag);
  }

  ms_prog_free(&qfrag);
  if (fails == 0) printf("PASS rational_holes: all Q-hole domain-typing checks\n");
  return fails ? 1 : 0;
}
