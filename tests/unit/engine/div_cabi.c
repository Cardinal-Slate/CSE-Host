/* Rational division through the C ABI: slate_dag_div builds (s+1)/(s+2) over a grid; the records are the reduced
 * fractions; the receipt's domain is Q; the int64 door answers "wide" for a non-integer cell; a fragment holding the
 * quotient saves, loads (the "rational" feature) and splices to the same cells. No C++ in this translation unit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL div_cabi: %s\n", msg); fails++; } } while (0)

typedef struct { uint8_t *buf; size_t len, cap; } MemSink;
static uint64_t mem_sink(const void *p, uint64_t n, void *u) {
  MemSink *m = (MemSink *)u;
  if (m->len + (size_t)n > m->cap) { m->cap = (m->len + (size_t)n) * 2 + 64; m->buf = (uint8_t *)realloc(m->buf, m->cap); }
  memcpy(m->buf + m->len, p, (size_t)n); m->len += (size_t)n; return n;
}
typedef struct { const uint8_t *buf; size_t len, pos; } MemSrc;
static uint64_t mem_src(void *p, uint64_t n, void *u) {
  MemSrc *m = (MemSrc *)u; size_t k = m->len - m->pos < (size_t)n ? m->len - m->pos : (size_t)n;
  memcpy(p, m->buf + m->pos, k); m->pos += k; return k;
}

/* read cell i of a run as (sign, num0, den0) through the records door */
static int cell(SlateArray *a, uint64_t i, uint64_t *sign, uint64_t *num0, uint64_t *den0) {
  uint64_t stride = 0, probe = 0;
  if (!slate_refused(slate_array_records(a, &probe, 0, &stride), "outsize") || !stride) return 0;
  uint64_t n = slate_array_size(a);
  uint64_t *rec = (uint64_t *)calloc((size_t)(n * stride), 8);
  const char *rc = slate_array_records(a, rec, n * stride * 8, &stride);
  if (rc) { free(rec); return 0; }
  const uint64_t *r = rec + i * stride; uint64_t L = r[2];
  *sign = r[1]; *num0 = r[3]; *den0 = r[3 + L];
  free(rec); return 1;
}

int main(void) {
  const int64_t N = 10;
  /* 1. the quotient, its records and its receipt */
  {
    SlateDag *b = slate_dag_new();
    int32_t s = slate_dag_param(b, 0);
    int32_t root = slate_dag_div(b, slate_dag_add(b, s, slate_dag_lit(b, 1)), slate_dag_add(b, s, slate_dag_lit(b, 2)));
    int64_t dims[1] = { N };
    SlateArray *a = slate_dag_run(b, root, dims, 1);
    CHECK(a != NULL, "run (s+1)/(s+2)");
    if (a) {
      for (int64_t i = 0; i < N; i++) {
        uint64_t sg = 9, nm = 0, dn = 0;
        int ok = cell(a, (uint64_t)i, &sg, &nm, &dn);
        /* gcd(i+1, i+2) = 1: the record is exactly (i+1)/(i+2) */
        CHECK(ok && sg == 0 && nm == (uint64_t)(i + 1) && dn == (uint64_t)(i + 2), "cell is (s+1)/(s+2), reduced");
      }
      const slate_entry *re; int32_t rn;
      CHECK(slate_array_receipt(a, &re, &rn) == NULL && slate_entry_is(re, rn, "domain", "Q"), "receipt domain is Q");
      CHECK(slate_entry_is(re, rn, "verdict", "exact"), "receipt verdict exact");
      int64_t num[16], den[16];
      CHECK(slate_array_i64_unsafe(a, num, den) == NULL && num[4] == 5 && den[4] == 6, "the int64 door reads the fraction as num/den");
      slate_array_free(a);
    }
    slate_dag_free(b);
  }
  /* 2. a fragment holding the quotient: save, load, splice, same cells */
  {
    SlateDag *b = slate_dag_new();
    int32_t s = slate_dag_param(b, 0);
    int32_t root = slate_dag_div(b, slate_dag_add(b, s, slate_dag_lit(b, 3)), slate_dag_add(b, slate_dag_mul(b, s, s), slate_dag_lit(b, 1)));
    MemSink out = { 0, 0, 0 };
    CHECK(slate_dag_save_fragment(b, root, NULL, 0, mem_sink, &out) == NULL, "save the quotient fragment");
    slate_dag_free(b);
    MemSrc src = { out.buf, out.len, 0 };
    SlateFrag *f = slate_frag_load(mem_src, &src);
    CHECK(f != NULL, "load the quotient fragment (feature \"rational\")");
    if (f) {
      SlateDag *b2 = slate_dag_new();
      int32_t r2 = -1;
      CHECK(slate_dag_splice(b2, f, NULL, 0, &r2) == NULL && r2 >= 0, "splice");
      int64_t dims[1] = { N };
      SlateArray *a = slate_dag_run(b2, r2, dims, 1);
      CHECK(a != NULL, "run the spliced quotient");
      if (a) {
        for (int64_t i = 0; i < N; i++) {
          uint64_t sg = 9, nm = 0, dn = 0; int ok = cell(a, (uint64_t)i, &sg, &nm, &dn);
          uint64_t en = (uint64_t)(i + 3), ed = (uint64_t)(i * i + 1), g = en, h = ed;   /* reduce (i+3)/(i²+1) */
          while (h) { uint64_t t = g % h; g = h; h = t; }
          CHECK(ok && sg == 0 && nm == en / g && dn == ed / g, "spliced cell is (s+3)/(s²+1), reduced");
        }
        slate_array_free(a);
      }
      slate_dag_free(b2);
      slate_frag_free(f);
    }
    free(out.buf);
  }
  if (fails) { printf("FAIL div_cabi: %d failure(s)\n", fails); return 1; }
  printf("PASS div_cabi: a quotient through the C ABI — reduced records, receipt domain Q, the int64 door reads num/den, a fragment with the rational feature round-trips\n");
  return 0;
}
