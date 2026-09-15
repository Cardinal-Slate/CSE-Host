/* tests/unit/engine/rational_sub_cabi.c — regression: `sub` (and any decompose-direction reduction) over
 * rational (RNS) carrier_q cells must recover the exact rational, never a wrong-but-exact value.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 *
 * The bug (fixed): detect_dot descended only Add, so a Sub-rooted reduction over RNS operands produced no
 * plan; dispatch fell to the positional int64 fallback, which laid the residue planes verbatim into an int64
 * plane and misread them — a wrong value stamped exact. The fix carries the sign as the PSDA decompose
 * direction (Λ) in the VecPlan so a−b rides the same residue lane as a+b (a−b = a+(−b) mod p), and a
 * fail-closed is_rns backstop refuses any residue-carrier shape that still can't ride that lane.
 *
 * Built and run like test_frag_cabi (pure C over the array C ABI). */
#include <stdio.h>
#include <stdint.h>
#include "slate/array.h"
#include "slate/batch.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  CHECK failed: %s\n", (msg)); fails++; } } while (0)

/* run root over a 1-cell grid; return the C ABI rc and (on OK) the cell's num/den. */
static int run1(SlateDag *d, int32_t root, int64_t *num, int64_t *den) {
  int64_t dims[1] = {1};
  SlateArray *a = slate_dag_run(d, root, dims, 1);
  if (!a) return SLATE_BATCH_EREFUSED;             /* a refused dispatch yields no array — fail-closed */
  int64_t nn[1], dd[1];
  int rc = slate_array_i64_unsafe(a, nn, dd);
  if (rc == SLATE_BATCH_OK) { *num = nn[0]; *den = dd[0]; }
  slate_array_free(a);
  return rc;
}

/* the reduction must be exact and equal wn/wd as a rational. */
static void expect_q(const char *name, SlateDag *d, int32_t root, int64_t wn, int64_t wd) {
  int64_t num = 0, den = 0;
  int rc = run1(d, root, &num, &den);
  CHECK(rc == SLATE_BATCH_OK, name);
  if (rc != SLATE_BATCH_OK) { printf("    %s: rc=%d (wanted %lld/%lld)\n", name, rc, (long long)wn, (long long)wd); return; }
  if (den < 0) { den = -den; num = -num; }
  int ok = ((__int128)num * wd == (__int128)wn * den);   /* num/den == wn/wd */
  CHECK(ok, name);
  if (!ok) printf("    %s: got %lld/%lld  wanted %lld/%lld\n", name, (long long)num, (long long)den, (long long)wn, (long long)wd);
}

/* the reduction's exact value exceeds int64 (or the shape can't ride the residue lane): it must fail-close
 * (a nonzero rc), never return a wrapped/misread int64 value stamped exact. */
static void expect_closed(const char *name, SlateDag *d, int32_t root) {
  int64_t num = 0, den = 0;
  int rc = run1(d, root, &num, &den);
  CHECK(rc != SLATE_BATCH_OK, name);
  if (rc == SLATE_BATCH_OK) printf("    %s: returned %lld/%lld — expected fail-closed (no wrong value)\n", name, (long long)num, (long long)den);
}

int main(void) {
  /* the exact reported bug: cells 1/3 and 22/7 over a common denominator 840. */
  {
    SlateDag *d = slate_dag_new();
    int64_t nums[2] = {280, 2640};                            /* 280/840 = 1/3 ; 2640/840 = 22/7 */
    uint32_t cq = slate_dag_carrier_q(d, nums, 2, 840, 64);
    int32_t a = slate_dag_load(d, cq, slate_dag_lit(d, 1));    /* 22/7 */
    int32_t b = slate_dag_load(d, cq, slate_dag_lit(d, 0));    /* 1/3  */
    expect_q("add(22/7,1/3)", d, slate_dag_add(d, a, b), 73, 21);   /* unchanged: compose */
    expect_q("mul(22/7,1/3)", d, slate_dag_mul(d, a, b), 22, 21);   /* unchanged: product */
    expect_q("sub(22/7,1/3)", d, slate_dag_sub(d, a, b), 59, 21);   /* the fix: decompose, exact */
    expect_q("sub(1/3,22/7)", d, slate_dag_sub(d, b, a), -59, 21);  /* negative result */
    slate_dag_free(d);
  }
  /* distinct denominators via a common denominator 12: 3/4 (9/12) and 5/6 (10/12). */
  {
    SlateDag *d = slate_dag_new();
    int64_t nums[2] = {9, 10};
    uint32_t cq = slate_dag_carrier_q(d, nums, 2, 12, 64);
    int32_t a = slate_dag_load(d, cq, slate_dag_lit(d, 0));    /* 3/4 */
    int32_t b = slate_dag_load(d, cq, slate_dag_lit(d, 1));    /* 5/6 */
    expect_q("add(3/4,5/6)", d, slate_dag_add(d, a, b), 19, 12);
    expect_q("sub(3/4,5/6)", d, slate_dag_sub(d, a, b), -1, 12);
    expect_q("sub(5/6,3/4)", d, slate_dag_sub(d, b, a), 1, 12);
    slate_dag_free(d);
  }
  /* wide cross-value: 5e18 - (-5e18) = 1e19 > INT64_MAX must fail-close, never wrap. */
  {
    SlateDag *d = slate_dag_new();
    int64_t nums[2] = {5000000000000000000LL, -5000000000000000000LL};
    uint32_t cq = slate_dag_carrier_q(d, nums, 2, 1, 96);
    int32_t a = slate_dag_load(d, cq, slate_dag_lit(d, 0));
    int32_t b = slate_dag_load(d, cq, slate_dag_lit(d, 1));
    expect_closed("sub_wide(1e19)", d, slate_dag_sub(d, a, b));
    slate_dag_free(d);
  }
  /* backstop: a residue-carrier shape detect_dot does not recognise (mulh) must refuse, not misread
   * residues as int64. */
  {
    SlateDag *d = slate_dag_new();
    int64_t nums[2] = {280, 2640};
    uint32_t cq = slate_dag_carrier_q(d, nums, 2, 840, 64);
    int32_t a = slate_dag_load(d, cq, slate_dag_lit(d, 1));
    int32_t b = slate_dag_load(d, cq, slate_dag_lit(d, 0));
    expect_closed("mulh_backstop", d, slate_dag_mulh(d, a, b));
    slate_dag_free(d);
  }

  if (fails == 0) printf("PASS rational_sub_cabi: sub over RNS carriers exact, backstop closed\n");
  return fails ? 1 : 0;
}
