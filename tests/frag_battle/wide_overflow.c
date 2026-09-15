/* wide_overflow: battle-test the wide/RNS readback path of the .slate array door.
 *
 * A fragment multiplies two large holes A[i]*B[i] so every cell exceeds 2^63 (int64). Verified in order:
 *   (1) run without a Potential set — record the behavior (refuse-NULL vs produce).
 *   (2) run with a Potential large enough — the wide result is produced (RNS lane).
 *   (3) slate_array_i64_unsafe on that wide result returns SLATE_BATCH_EWIDE (7), not a
 *       wrong/truncated value.
 *   (4) slate_array_records reconstructs the exact bignum for every cell (checked against a
 *       __int128 value computed here by hand).
 *   (5) provisioning edges: potential too small, potential far larger, a cube that needs 2 limbs.
 *
 * Every numeric result is checked against a hand-computed value. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "slate/array.h"
#include "slate/stream.h"

/* growable memory sink + cursor source (same pattern as frag_cabi.c) */
typedef struct { unsigned char *buf; size_t len, cap; } MemSink;
static uint64_t mem_sink(const void *bytes, uint64_t n, void *user) {
  MemSink *m = (MemSink *)user;
  if (m->len + n > m->cap) { m->cap = (m->len + n) * 2 + 64; m->buf = (unsigned char *)realloc(m->buf, m->cap); }
  memcpy(m->buf + m->len, bytes, (size_t)n); m->len += (size_t)n; return n;
}
typedef struct { const unsigned char *buf; size_t len, pos; } MemSrc;
static uint64_t mem_src(void *bytes, uint64_t n, void *user) {
  MemSrc *s = (MemSrc *)user;
  if (s->pos + n > s->len) return 0;
  memcpy(bytes, s->buf + s->pos, (size_t)n); s->pos += (size_t)n; return n;
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL wide_overflow: %s\n", msg); fails++; } \
                              else { printf("  ok: %s\n", msg); } } while (0)

typedef unsigned __int128 u128;
static void print_u128(u128 v) {           /* decimal print for diagnostics */
  char tmp[40]; int n = 0;
  if (v == 0) { printf("0"); return; }
  while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
  while (n) putchar(tmp[--n]);
}

/* Decode record cell `i` (layout [valid,sign,L,|num|LE,den LE], stride 3+2L) into
 * unsigned-128 magnitude + sign + den. Returns 0 on success; asserts L<=2 fits u128. */
static int decode_cell(const uint64_t *rec, uint64_t stride, uint64_t i,
                       int *valid, int *sign, u128 *mag, u128 *den) {
  const uint64_t *w = rec + i * stride;
  *valid = (int)w[0];
  *sign  = (int)w[1];
  uint64_t L = w[2];
  if (L > 2) { printf("  (cell %llu L=%llu > 2, u128 decode won't fit)\n",
                      (unsigned long long)i, (unsigned long long)L); return -1; }
  u128 m = 0, d = 0;
  for (uint64_t j = 0; j < L; j++) m |= (u128)w[3 + j] << (64 * j);
  for (uint64_t j = 0; j < L; j++) d |= (u128)w[3 + L + j] << (64 * j);
  *mag = m; *den = d;
  return 0;
}

/* Build a fragment root[i] = A[i]*B[i] with A,B as holes; serialize to `out`. */
static void build_product_fragment(MemSink *out, uint64_t n) {
  SlateDag *b = slate_dag_new();
  int64_t *ph = (int64_t *)calloc(n, sizeof(int64_t));   /* placeholder data to build+typecheck once */
  uint32_t cidA = slate_dag_carrier(b, ph, n);
  uint32_t cidB = slate_dag_carrier(b, ph, n);
  int32_t p = slate_dag_param(b, 0);
  int32_t lA = slate_dag_load(b, cidA, p);
  int32_t lB = slate_dag_load(b, cidB, p);
  int32_t root = slate_dag_mul(b, lA, lB);
  uint32_t holes[2] = {cidA, cidB};
  int rc = slate_dag_save_fragment(b, root, holes, 2, mem_sink, out);
  CHECK(rc == SLATE_BATCH_OK, "save 2-hole product fragment");
  slate_dag_free(b);
  free(ph);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const uint64_t N = 4;

  /* Wide inputs: every A[i]*B[i] exceeds 2^63 (= 9223372036854775808). Cell 3 uses
   * 3037000500 whose square is only just above 2^63 — the tightest overflow edge. */
  int64_t A[4] = { 5000000000LL, 6000000000LL, 7000000000LL, 3037000500LL };
  int64_t B[4] = { 5000000000LL, 6000000000LL, 7000000000LL, 3037000500LL };
  u128 expect[4];
  for (int i = 0; i < 4; i++) expect[i] = (u128)A[i] * (u128)B[i];

  printf("expected products (all must exceed 2^63 = 9223372036854775808):\n");
  for (int i = 0; i < 4; i++) { printf("  cell %d = ", i); print_u128(expect[i]); printf("\n"); }
  /* sanity: confirm my inputs really do overflow int64 */
  for (int i = 0; i < 4; i++) CHECK(expect[i] > (u128)INT64_MAX, "hand product exceeds int64");

  MemSink frag = {0};
  build_product_fragment(&frag, N);
  CHECK(frag.len > 0, "fragment bytes nonempty");

  int64_t dims[1] = { (int64_t)N };

  /* -------- (1) run without a Potential set: observe refuse vs produce -------- */
  int no_potential_produced = 0;
  {
    MemSrc src = {frag.buf, frag.len, 0};
    SlateFrag *f = slate_frag_load(mem_src, &src);
    CHECK(f != NULL, "frag load (no-potential run)");
    SlateDag *b = slate_dag_new();
    uint32_t cA = slate_dag_carrier(b, A, N);
    uint32_t cB = slate_dag_carrier(b, B, N);
    uint32_t args[2] = {cA, cB};
    int32_t root = slate_dag_splice(b, f, args, 2);
    CHECK(root >= 0, "splice ok (no-potential run)");
    SlateArray *a = slate_dag_run(b, root, dims, 1);
    if (a) {
      no_potential_produced = 1;
      printf("  FINDING: run WITHOUT explicit Potential produced a wide array (auto-provisioned)\n");
      int64_t num[4], den[4];
      int rc = slate_array_i64_unsafe(a, num, den);
      CHECK(rc == SLATE_BATCH_EWIDE, "no-potential: i64_unsafe -> EWIDE(7)");
      slate_array_free(a);
    } else {
      printf("  FINDING: run WITHOUT explicit Potential REFUSED (NULL) — Potential is required for wide cells\n");
    }
    slate_dag_free(b);
    slate_frag_free(f);
  }

  /* -------- (2)+(3)+(4) run with a large Potential: produce, EWIDE, exact records -------- */
  {
    MemSrc src = {frag.buf, frag.len, 0};
    SlateFrag *f = slate_frag_load(mem_src, &src);
    SlateDag *b = slate_dag_new();
    int prc = slate_dag_potential(b, 128);          /* 128-bit ceiling >> ~66-bit products */
    CHECK(prc == SLATE_BATCH_OK, "slate_dag_potential(128) ok");
    uint32_t cA = slate_dag_carrier(b, A, N);
    uint32_t cB = slate_dag_carrier(b, B, N);
    uint32_t args[2] = {cA, cB};
    int32_t root = slate_dag_splice(b, f, args, 2);
    CHECK(root >= 0, "splice ok (potential run)");
    SlateArray *a = slate_dag_run(b, root, dims, 1);
    CHECK(a != NULL, "wide dispatch PRODUCED a result under Potential=128");
    if (a) {
      CHECK(slate_array_size(a) == N, "result size == N");
      slate_reading rd; slate_array_receipt(a, &rd);
      printf("  receipt: exact=%d refused=%d domain=%d path=%d mode=%d work=%llu\n",
             rd.exact, rd.refused, rd.domain, rd.path, rd.mode, (unsigned long long)rd.work);
      CHECK(rd.exact == 1 && rd.refused == 0, "receipt: exact, not refused");

      /* (3) the EWIDE refusal path of the int64 pull */
      int64_t num[4], den[4];
      int rc = slate_array_i64_unsafe(a, num, den);
      CHECK(rc == SLATE_BATCH_EWIDE, "i64_unsafe -> SLATE_BATCH_EWIDE(7), not a wrong value");

      /* (4) exact bignum readback via records: size-then-fill */
      uint64_t stride = 0, probe = 0;
      /* stride probe: a NULL out now returns EOUTSIZE with *out_stride set (size-then-fill), matching
       * slate_frag_iface/peek NULL-probe ergonomics. */
      int rnull = slate_array_records(a, NULL, 0, &stride);
      CHECK(rnull == SLATE_BATCH_EOUTSIZE && stride >= 3, "records: NULL out -> EOUTSIZE stride probe");
      int r0 = slate_array_records(a, &probe, 0, &stride);
      CHECK(r0 == SLATE_BATCH_EOUTSIZE, "records size-then-fill: EOUTSIZE on 0 bytes (non-NULL out)");
      CHECK(stride >= 5, "records stride >= 5 (3 + 2*L, L>=1)");
      printf("  records stride = %llu (=> L = %llu)\n",
             (unsigned long long)stride, (unsigned long long)((stride - 3) / 2));
      uint64_t bytes = N * stride * sizeof(uint64_t);
      uint64_t *rec = (uint64_t *)malloc(bytes);
      uint64_t stride2 = 0;
      int r1 = slate_array_records(a, rec, bytes, &stride2);
      CHECK(r1 == SLATE_BATCH_OK && stride2 == stride, "records filled OK, stride stable");

      for (uint64_t i = 0; i < N; i++) {
        int valid, sign; u128 mag, den2;
        int dr = decode_cell(rec, stride, i, &valid, &sign, &mag, &den2);
        CHECK(dr == 0, "cell decode fit u128");
        CHECK(valid == 1, "cell valid");
        CHECK(sign == 0, "cell sign positive");
        CHECK(den2 == 1, "cell denominator == 1 (integer)");
        int match = (mag == expect[i]);
        if (!match) { printf("    cell %llu: got ", (unsigned long long)i); print_u128(mag);
                      printf(" expected "); print_u128(expect[i]); printf("\n"); }
        CHECK(match, "cell magnitude EXACT vs hand-computed product");
      }
      free(rec);
      slate_array_free(a);
    }
    slate_dag_free(b);
    slate_frag_free(f);
  }

  /* -------- (5a) Potential far larger than needed: still exact (over-provision is safe) -------- */
  {
    MemSrc src = {frag.buf, frag.len, 0};
    SlateFrag *f = slate_frag_load(mem_src, &src);
    SlateDag *b = slate_dag_new();
    slate_dag_potential(b, 512);
    uint32_t cA = slate_dag_carrier(b, A, N);
    uint32_t cB = slate_dag_carrier(b, B, N);
    uint32_t args[2] = {cA, cB};
    int32_t root = slate_dag_splice(b, f, args, 2);
    SlateArray *a = slate_dag_run(b, root, dims, 1);
    CHECK(a != NULL, "over-provisioned (P=512) still produces");
    if (a) {
      uint64_t stride = 0, probe = 0; slate_array_records(a, &probe, 0, &stride);
      uint64_t bytes = N * stride * sizeof(uint64_t);
      uint64_t *rec = (uint64_t *)malloc(bytes);
      slate_array_records(a, rec, bytes, &stride);
      int allok = 1;
      for (uint64_t i = 0; i < N; i++) {
        int v, s; u128 m, d; decode_cell(rec, stride, i, &v, &s, &m, &d);
        if (!(v == 1 && s == 0 && d == 1 && m == expect[i])) allok = 0;
      }
      CHECK(allok, "over-provisioned records still EXACT");
      free(rec);
      slate_array_free(a);
    }
    slate_dag_free(b);
    slate_frag_free(f);
  }

  /* -------- (5b) a cube A[i]^3 that needs 2 limbs (value ~1.25e29) — plain build, Potential=256 -------- */
  {
    SlateDag *b = slate_dag_new();
    slate_dag_potential(b, 256);
    int64_t C[4] = { 5000000000LL, 6000000000LL, 7000000000LL, 9000000000LL };
    u128 cexp[4];
    for (int i = 0; i < 4; i++) cexp[i] = (u128)C[i] * (u128)C[i] * (u128)C[i];
    uint32_t cid = slate_dag_carrier(b, C, N);
    int32_t p = slate_dag_param(b, 0);
    int32_t l = slate_dag_load(b, cid, p);
    int32_t sq = slate_dag_mul(b, l, l);
    int32_t cube = slate_dag_mul(b, sq, l);          /* C[i]^3 */
    SlateArray *a = slate_dag_run(b, cube, dims, 1);
    CHECK(a != NULL, "cube dispatch produced under P=256");
    if (a) {
      int64_t num[4], den[4];
      CHECK(slate_array_i64_unsafe(a, num, den) == SLATE_BATCH_EWIDE, "cube i64_unsafe -> EWIDE");
      uint64_t stride = 0, probe = 0; slate_array_records(a, &probe, 0, &stride);
      printf("  cube records stride = %llu (L = %llu)\n",
             (unsigned long long)stride, (unsigned long long)((stride - 3) / 2));
      uint64_t bytes = N * stride * sizeof(uint64_t);
      uint64_t *rec = (uint64_t *)malloc(bytes);
      slate_array_records(a, rec, bytes, &stride);
      for (uint64_t i = 0; i < N; i++) {
        int v, s; u128 m, d; decode_cell(rec, stride, i, &v, &s, &m, &d);
        int match = (v == 1 && s == 0 && d == 1 && m == cexp[i]);
        if (!match) { printf("    cube cell %llu: got ", (unsigned long long)i); print_u128(m);
                      printf(" expected "); print_u128(cexp[i]); printf("\n"); }
        CHECK(match, "cube cell magnitude EXACT");
      }
      free(rec);
      slate_array_free(a);
    }
    slate_dag_free(b);
  }

  /* -------- (5c) Potential too small: under-provision must not yield a wrong value -------- */
  {
    SlateDag *b = slate_dag_new();
    int prc = slate_dag_potential(b, 8);             /* 8 bits << ~66-bit product */
    CHECK(prc == SLATE_BATCH_OK, "potential(8) accepted");
    uint32_t cA = slate_dag_carrier(b, A, N);
    uint32_t cB = slate_dag_carrier(b, B, N);
    int32_t p = slate_dag_param(b, 0);
    int32_t root = slate_dag_mul(b, slate_dag_load(b, cA, p), slate_dag_load(b, cB, p));
    SlateArray *a = slate_dag_run(b, root, dims, 1);
    if (!a) {
      printf("  FINDING: under-provisioned (P=8) run REFUSED (NULL) — safe\n");
      CHECK(1, "under-provision refuses cleanly (NULL)");
    } else {
      /* If it produced, every cell must be either refused(records valid=0) or exact — never wrong. */
      uint64_t stride = 0, probe = 0; int r0 = slate_array_records(a, &probe, 0, &stride);
      if (r0 == SLATE_BATCH_EOUTSIZE) {
        uint64_t bytes = N * stride * sizeof(uint64_t);
        uint64_t *rec = (uint64_t *)malloc(bytes);
        slate_array_records(a, rec, bytes, &stride);
        int safe = 1;
        for (uint64_t i = 0; i < N; i++) {
          int v, s; u128 m, d; int dr = decode_cell(rec, stride, i, &v, &s, &m, &d);
          if (v == 1 && dr == 0) {             /* claims a value -> it must be the exact one */
            if (!(s == 0 && d == 1 && m == expect[i])) { safe = 0;
              printf("    UNSAFE under-provision cell %llu: got ", (unsigned long long)i);
              print_u128(m); printf(" expected "); print_u128(expect[i]); printf("\n"); }
          }
        }
        CHECK(safe, "under-provision: any valid cell is EXACT (no wrong value)");
        free(rec);
      } else {
        printf("  under-provision produced array; records rc=%d\n", r0);
      }
      slate_reading rd; slate_array_receipt(a, &rd);
      printf("  under-provision receipt: exact=%d refused=%d\n", rd.exact, rd.refused);
      slate_array_free(a);
    }
    slate_dag_free(b);
  }

  free(frag.buf);
  printf(no_potential_produced ? "(note: no-Potential run auto-provisioned)\n"
                               : "(note: no-Potential run refused)\n");
  if (fails == 0) printf("PASS wide_overflow: all wide/EWIDE/records/Potential checks\n");
  else            printf("FAILED wide_overflow: %d check(s)\n", fails);
  return fails ? 1 : 0;
}
