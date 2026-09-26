/* tests/unit/abi_slate.c — the C ABI, user band (slate/slate.h), from pure C. No C++ in this translation unit.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 *
 * Gate: two binaries are two numbers and their relationship is one rational in Q — the reading of div is the reduced
 * fraction, whatever the size of the binaries; every global question about the pair is a sign question answered by
 * a reading, never by recomposing; the store is on the seam — the same construction in a second builder is a hit
 * with no rows put; a fragment is the interchange unit — kept as a leaf, loaded back by name in another
 * container over the same store, spliced over new binaries, and invoked by name; a refusal is a name, never a
 * wrong value. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- binaries: little-endian byte strings; ×k in place (k small) ---- */
static size_t mul_small(uint8_t *b, size_t n, unsigned k, size_t cap) {
  unsigned carry = 0;
  for (size_t i = 0; i < n; i++) { unsigned v = b[i] * k + carry; b[i] = (uint8_t)v; carry = v >> 8; }
  while (carry && n < cap) { b[n++] = (uint8_t)carry; carry >>= 8; }
  return n;
}
static void random_bytes(uint8_t *b, size_t n, uint64_t seed) {
  for (size_t i = 0; i < n; i++) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; b[i] = (uint8_t)(seed >> 56); }
  if (n) b[n - 1] |= 0x81;          /* a top byte ≥ 128: ×2..×7 carry into exactly one more byte */
}

/* a binary is a number: Horner over its byte cells, most significant first */
static int32_t number(SlateDag *b, uint32_t cid, size_t n) {
  int32_t x = slate_dag_lit(b, 0), k = slate_dag_lit(b, 256);
  for (size_t i = n; i-- > 0;) x = slate_dag_add(b, slate_dag_mul(b, x, k), slate_dag_load(b, cid, slate_dag_lit(b, (int64_t)i)));
  return x;
}

/* the reading of a one-cell result: verdict/domain off the receipt, sign/num/den off the lossless record. Returns
   0 on an exact single-limb reading, else -1 (refused, wide, or not run). */
typedef struct { int sign; uint64_t num, den; uint64_t limbs; const char *domain; } Reading;
static int read1(SlateArray *a, Reading *r) {
  memset(r, 0, sizeof *r); r->domain = "";
  if (!a) return -1;
  const slate_entry *e; int32_t n;
  if (slate_array_receipt(a, &e, &n) != NULL) return -1;
  if (!slate_entry_is(e, n, "verdict", "exact")) return -1;
  const slate_entry *d = slate_entry_find(e, n, "domain"); if (d) r->domain = (const char *)d->bytes;
  uint64_t stride = 0;
  if (!slate_refused(slate_array_records(a, NULL, 0, &stride), "outsize") || stride < 5) return -1;
  uint64_t *rec = (uint64_t *)malloc((size_t)stride * 8);
  if (slate_array_records(a, rec, stride * 8, &stride) != NULL) { free(rec); return -1; }
  uint64_t L = rec[2]; r->limbs = L; r->sign = (int)rec[1];
  int ok = rec[0] == 1;
  for (uint64_t i = 1; i < L; i++) if (rec[3 + i] || rec[3 + L + i]) ok = 0;   /* single-limb only */
  r->num = rec[3]; r->den = rec[3 + L];
  free(rec);
  return ok ? 0 : -1;
}
static int reads(SlateDag *b, int32_t root, int sign, uint64_t num, uint64_t den, const char *domain, const char *what) {
  int64_t one = 1; SlateArray *a = slate_dag_run(b, root, &one, 1);
  Reading r; int ok = read1(a, &r) == 0 && r.sign == sign && r.num == num && r.den == den && (!domain || strcmp(r.domain, domain) == 0);
  if (!ok) { fails++; printf("  FAIL %s: read %s%llu/%llu (%s) wanted %s%llu/%llu\n", what, a ? (r.sign ? "-" : "") : "no array ", (unsigned long long)r.num, (unsigned long long)r.den, r.domain, sign ? "-" : "", (unsigned long long)num, (unsigned long long)den); }
  slate_array_free(a);
  return ok;
}

/* ---- the caller's store: (word, bytes) rows behind the three callbacks, counting ---- */
typedef struct { uint8_t *w; uint64_t wn; uint8_t *b; uint64_t n; } Row;
typedef struct { Row *r; size_t n, cap; long gets, hits, puts; } Store;
static pthread_mutex_t st_mu = PTHREAD_MUTEX_INITIALIZER;   /* the engine calls a store from several threads */
static int st_get(const uint8_t *w, uint64_t wn, const uint8_t *s, uint64_t sn, uint8_t **out, uint64_t *outn, void *u) {
  (void)s; (void)sn; Store *st = (Store *)u; int rc = 1;
  pthread_mutex_lock(&st_mu); st->gets++;
  for (size_t i = 0; i < st->n; i++) if (st->r[i].wn == wn && memcmp(st->r[i].w, w, (size_t)wn) == 0) {
    st->hits++; *out = (uint8_t *)malloc(st->r[i].n ? (size_t)st->r[i].n : 1); memcpy(*out, st->r[i].b, (size_t)st->r[i].n); *outn = st->r[i].n; rc = 0; break; }
  pthread_mutex_unlock(&st_mu);
  return rc;
}
static int st_put(const uint8_t *w, uint64_t wn, const uint8_t *b, uint64_t n, const uint8_t *s, uint64_t sn, void *u) {
  (void)s; (void)sn; Store *st = (Store *)u;
  pthread_mutex_lock(&st_mu); st->puts++;
  for (size_t i = 0; i < st->n; i++) if (st->r[i].wn == wn && memcmp(st->r[i].w, w, (size_t)wn) == 0) {
    free(st->r[i].b); st->r[i].b = (uint8_t *)malloc(n ? (size_t)n : 1); memcpy(st->r[i].b, b, (size_t)n); st->r[i].n = n;
    pthread_mutex_unlock(&st_mu); return 0; }
  if (st->n == st->cap) { st->cap = st->cap ? st->cap * 2 : 64; st->r = (Row *)realloc(st->r, st->cap * sizeof(Row)); }
  Row *r = &st->r[st->n++];
  r->w = (uint8_t *)malloc((size_t)wn); memcpy(r->w, w, (size_t)wn); r->wn = wn;
  r->b = (uint8_t *)malloc(n ? (size_t)n : 1); memcpy(r->b, b, (size_t)n); r->n = n;
  pthread_mutex_unlock(&st_mu);
  return 0;
}
static void st_free(Store *st) { for (size_t i = 0; i < st->n; i++) { free(st->r[i].w); free(st->r[i].b); } free(st->r); memset(st, 0, sizeof *st); }

/* the construction under test, over the binaries A = j·G and B = k·G registered in `b` */
typedef struct { uint32_t ca, cb, cg; int32_t A, B, G; } Pair;
static Pair build(SlateDag *b, const uint8_t *G, size_t ng, unsigned j, unsigned k) {
  uint8_t A[520], B[520]; memcpy(A, G, ng); memcpy(B, G, ng);
  size_t na = mul_small(A, ng, j, sizeof A), nb = mul_small(B, ng, k, sizeof B);
  Pair p; p.ca = slate_dag_carrier_bytes(b, A, na); p.cb = slate_dag_carrier_bytes(b, B, nb); p.cg = slate_dag_carrier_bytes(b, G, ng);
  p.A = number(b, p.ca, na); p.B = number(b, p.cb, nb); p.G = number(b, p.cg, ng);
  return p;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  static const size_t sizes[] = { 3, 40, 200 };

  /* ---- 1. two binaries are two numbers; their relation is one rational in Q, whatever their size ---- */
  for (size_t s = 0; s < 3; s++) {
    size_t ng = sizes[s]; uint8_t G[200]; random_bytes(G, ng, 7 + ng);
    SlateDag *b = slate_dag_new(); CHECK(b, "1: no builder");
    Pair p = build(b, G, ng, 3, 5);
    reads(b, slate_dag_div(b, p.A, p.B), 0, 3, 5, "Q", "1: A/B at 3·G, 5·G");                         /* the relation: 3/5 */
    reads(b, slate_dag_div(b, slate_dag_add(b, p.A, p.B), p.G), 0, 8, 1, NULL, "1: (A+B)/G");            /* = 8 */
    reads(b, slate_dag_div(b, slate_dag_mul(b, p.A, p.B), slate_dag_mul(b, p.G, p.G)), 0, 15, 1, NULL, "1: A·B/G²");   /* = 15 */
    reads(b, slate_dag_div(b, slate_dag_sub(b, p.B, p.A), p.G), 0, 2, 1, NULL, "1: (B−A)/G");            /* = 2 */
    slate_dag_free(b);
    printf("  relation: %zu-byte binaries — A/B reads 3/5, (A+B)/G 8, A·B/G² 15, (B−A)/G 2\n", ng);
  }

  /* ---- 2. every global question is a sign question: order is the sign of the difference, read off the record ---- */
  {
    uint8_t G[40]; random_bytes(G, 40, 11);
    SlateDag *b = slate_dag_new(); Pair p = build(b, G, 40, 3, 5);
    int64_t one = 1; SlateArray *a;
    Reading r;
    a = slate_dag_run(b, slate_dag_sub(b, p.A, p.B), &one, 1); CHECK(a && read1(a, &r) != 0 && r.sign == 1, "2: A<B — the difference's sign channel is not negative"); slate_array_free(a);
    a = slate_dag_run(b, slate_dag_sub(b, p.B, p.A), &one, 1); CHECK(a && read1(a, &r) != 0 && r.sign == 0, "2: B>A — the difference's sign channel is not positive"); slate_array_free(a);
    reads(b, slate_dag_sub(b, p.A, p.A), 0, 0, 1, NULL, "2: A−A");                                       /* zero has no sign */
    /* a wide reading is lossless: the record carries L limbs, and (A−B) is −2G — its magnitude is G doubled */
    a = slate_dag_run(b, slate_dag_sub(b, p.A, p.B), &one, 1);
    { uint64_t stride = 0; slate_array_records(a, NULL, 0, &stride); uint64_t *rec = (uint64_t *)malloc((size_t)stride * 8);
      CHECK(slate_array_records(a, rec, stride * 8, &stride) == NULL, "2: records refused");
      uint64_t L = rec[2]; uint8_t twoG[48]; memcpy(twoG, G, 40); size_t n2 = mul_small(twoG, 40, 2, 48);
      uint8_t got[48] = {0}; for (uint64_t i = 0; i < L && i < 6; i++) for (int k = 0; k < 8; k++) got[i * 8 + k] = (uint8_t)(rec[3 + i] >> (8 * k));
      CHECK(rec[1] == 1 && memcmp(got, twoG, n2) == 0, "2: |A−B| is not 2·G limb for limb");
      CHECK(slate_refused(slate_array_i64_unsafe(a, (int64_t[]){0}, (int64_t[]){0}), "wide"), "2: a wide value did not answer \"wide\" on the int64 door");
      free(rec); }
    slate_array_free(a); slate_dag_free(b);
    printf("  sign: A<B, B>A, A−A = 0; |A−B| = 2·G limb for limb; the int64 door answers \"wide\", never a wrong value\n");
  }

  /* ---- 3. the store is on the seam: the same construction in a second builder is a hit, no rows put ---- */
  {
    uint8_t G[40]; random_bytes(G, 40, 13);
    Store st; memset(&st, 0, sizeof st);
    SlateDag *b1 = slate_dag_new(); CHECK(slate_dag_codec(b1, NULL, st_get, st_put, NULL, 0, &st) == NULL, "3: codec refused");
    Pair p1 = build(b1, G, 40, 3, 5); reads(b1, slate_dag_div(b1, p1.A, p1.B), 0, 3, 5, "Q", "3: first ask");
    long puts1 = st.puts; CHECK(puts1 > 0, "3: the first ask put no rows");
    SlateDag *b2 = slate_dag_new(); slate_dag_codec(b2, NULL, st_get, st_put, NULL, 0, &st);
    Pair p2 = build(b2, G, 40, 3, 5); long h0 = st.hits; reads(b2, slate_dag_div(b2, p2.A, p2.B), 0, 3, 5, "Q", "3: second ask");
    CHECK(st.puts == puts1 && st.hits > h0, "3: the second builder put %ld rows / hit %ld (want 0 / ≥1)", st.puts - puts1, st.hits - h0);
    /* a different relation over the same binaries is a different word: it runs (puts) */
    long puts2 = st.puts; reads(b2, slate_dag_div(b2, p2.B, p2.A), 0, 5, 3, "Q", "3: B/A"); CHECK(st.puts > puts2, "3: a new relation put no row");
    slate_dag_free(b1); slate_dag_free(b2);
    printf("  store: first ask %ld rows put; the same construction in a new builder 0 put, hit; a new relation puts\n", puts1);
    st_free(&st);
  }

  /* ---- 4. a fragment is the interchange unit: keep it as a leaf with the binaries as holes, walk it back by
     its word in a fresh container over the same store, splice over new ones, invoke ---- */
  {
    uint8_t G[40]; random_bytes(G, 40, 17);
    Store st; memset(&st, 0, sizeof st);
    SlateDag *b = slate_dag_new(); slate_dag_codec(b, NULL, st_get, st_put, NULL, 0, &st);
    Pair p = build(b, G, 40, 3, 5);
    int32_t root = slate_dag_div(b, p.A, p.B);
    uint8_t *fw = NULL; uint64_t fwn = 0; uint32_t holes[2] = { p.ca, p.cb };
    CHECK(slate_dag_save_fragment(b, root, holes, 2, &fw, &fwn) == NULL, "4: keep refused");
    slate_dag_free(b);
    /* another container, the same store: a program one kept is loaded by name by another */
    SlateDag *lb = slate_dag_new(); slate_dag_codec(lb, NULL, st_get, st_put, NULL, 0, &st);
    SlateFrag *f = slate_frag_load(lb, fw, fwn); CHECK(f, "4: load refused");
    uint32_t np = 0, nh = 0; slate_iface_receipt rr; CHECK(slate_frag_iface(f, &np, NULL, &nh, &rr) == NULL && nh == 2, "4: iface: %u holes (want 2)", nh);
    CHECK(rr.kind && strcmp(rr.kind, "scalar") == 0 && (!rr.domain || strcmp(rr.domain, "Q") == 0), "4: root receipt %s/%s (want scalar, Q or derived)", rr.kind ? rr.kind : "?", rr.domain ? rr.domain : "derived");
    /* splice over new binaries A' = 7·G', B' = 2·G' of the same lengths (the record reads a fixed window of each hole —
       a binary of another length is another record): the same relation reads 7/2 */
    uint8_t G2[40]; random_bytes(G2, 40, 19); uint8_t A2[48], B2[48]; memcpy(A2, G2, 40); memcpy(B2, G2, 40);
    size_t na = mul_small(A2, 40, 7, 48), nb = mul_small(B2, 40, 2, 48);
    SlateDag *c = slate_dag_new(); slate_dag_codec(c, NULL, st_get, st_put, NULL, 0, &st);
    uint32_t args[2] = { slate_dag_carrier_bytes(c, A2, na), slate_dag_carrier_bytes(c, B2, nb) };
    int32_t r2 = -1; CHECK(slate_dag_splice(c, f, args, 2, &r2) == NULL && r2 >= 0, "4: splice refused");
    reads(c, r2, 0, 7, 2, "Q", "4: spliced A'/B'");
    /* reflection: the spliced root is a div over two ops */
    { const char *kind = NULL, *op = NULL; int32_t x = -1, y = -1; CHECK(slate_dag_node(c, r2, &kind, &op, &x, &y, NULL, NULL) == NULL && kind && strcmp(kind, "op") == 0 && op && strcmp(op, "div") == 0 && x >= 0 && y >= 0, "4: the root is not a div"); }
    slate_dag_free(c);
    /* invoke by name: the program's name and the two binaries, one call, in a container over the same store */
    { const uint8_t *av[2] = { A2, B2 }; uint64_t al[2] = { na, nb };
      SlateDag *iv = slate_dag_new(); slate_dag_codec(iv, NULL, st_get, st_put, NULL, 0, &st);
      SlateArray *a = slate_invoke(iv, fw, fwn, av, al, 2, NULL, 0, NULL, 0); Reading r;
      CHECK(a && read1(a, &r) == 0 && r.sign == 0 && r.num == 7 && r.den == 2, "4: invoke read %llu/%llu", (unsigned long long)r.num, (unsigned long long)r.den);
      slate_array_free(a); slate_dag_free(iv); }
    /* a word no store holds is a miss: nothing to invoke */
    { uint8_t *junk = (uint8_t *)malloc((size_t)fwn); for (uint64_t i = 0; i < fwn; i++) junk[i] = (uint8_t)(fw[i] ^ 0xA5);
      SlateDag *iv = slate_dag_new(); slate_dag_codec(iv, NULL, st_get, st_put, NULL, 0, &st);
      CHECK(slate_invoke(iv, junk, fwn, NULL, NULL, 0, NULL, 0, NULL, 0) == NULL, "4: an unkept name was invoked");
      slate_dag_free(iv); free(junk); }
    /* wrong arity refuses before any node is emitted */
    { SlateDag *d = slate_dag_new(); int32_t r3 = 0; CHECK(slate_refused(slate_dag_splice(d, f, args, 1, &r3), "args") && r3 == -1, "4: wrong arity not refused"); slate_dag_free(d); }
    slate_frag_free(f); slate_dag_free(lb);
    printf("  fragment: A/B kept with two holes (one cell per piece, walked back), loaded by its word (scalar), spliced over 7·G'/2·G' reads 7/2, invoked by its word reads 7/2\n");
    free(fw); st_free(&st);
  }

  /* ---- 6. refusals are names, never values ---- */
  {
    SlateDag *b = slate_dag_new(); uint8_t G[8]; random_bytes(G, 8, 29); Pair p = build(b, G, 8, 3, 5);
    int64_t one = 1; SlateArray *a = slate_dag_run(b, slate_dag_div(b, p.A, slate_dag_lit(b, 0)), &one, 1);
    if (a) { const slate_entry *e; int32_t n; slate_array_receipt(a, &e, &n); CHECK(slate_entry_is(e, n, "verdict", "undefined"), "6: ÷0 has a verdict other than undefined");
             CHECK(slate_refused(slate_array_i64_unsafe(a, (int64_t[]){0}, (int64_t[]){0}), "refused"), "6: ÷0 cell not refused on the int64 door"); slate_array_free(a); }
    CHECK(slate_refused(slate_dag_potential(NULL, 64), "args"), "6: null builder not \"args\"");
    CHECK(slate_dag_load(b, 99, p.A) == -1 && slate_dag_run(b, p.A, &one, 1) == NULL, "6: a bad carrier id did not poison the builder");
    slate_dag_free(b);
    printf("  refusals: ÷0 reads undefined and \"refused\"; a null builder \"args\"; a bad id poisons the builder and run answers NULL\n");
  }

  if (fails) { printf("FAIL test_abi_slate: %d checks failed\n", fails); return 1; }
  printf("PASS test_abi_slate: two binaries one relation in Q; order a sign; the store on the seam; a fragment kept as a leaf, loaded by name, spliced, invoked; refusals named\n");
  return 0;
}
