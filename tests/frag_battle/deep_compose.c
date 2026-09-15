/* deep_compose: compose .slate fragments 3+ levels deep and verify receipts stay coherent.
 *
 *   Level 1 (F):  y[i] = A[i]*2                (A a hole)
 *   Level 2 (G):  splice F, then g = F + B     (A,B holes) -> save G as a new fragment
 *   Level 3     :  load G, splice it, final = G + C, run
 *
 * Final exact value: final[i] = A[i]*2 + B[i] + C[i].
 * Every intermediate and final exact value is checked by hand and every run's receipt asserted exact==1.
 * Exercises a fragment-of-a-fragment (G contains a spliced F), receipt composition across two splice
 * seams, and re-saving a spliced result as a new fragment.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/array.h"
#include "slate/stream.h"

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
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL deep_compose: %s\n", msg); fails++; } \
                              else { printf("PASS deep_compose: %s\n", msg); } } while (0)

#define N 4

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);

  int64_t A[N] = {1, 2, 3, 4};
  int64_t B[N] = {10, 20, 30, 40};
  int64_t C[N] = {100, 200, 300, 400};
  /* hand-computed expectations */
  int64_t exp_F[N]     = {2, 4, 6, 8};            /* A*2 */
  int64_t exp_G[N]     = {12, 24, 36, 48};        /* A*2 + B */
  int64_t exp_final[N] = {112, 224, 336, 448};    /* A*2 + B + C */

  /* ---------- Level 1: build F (y = A*2), save it ---------- */
  MemSink fragF = {0};
  {
    SlateDag *b = slate_dag_new();
    int64_t placeholderA[N] = {0};
    uint32_t cidA = slate_dag_carrier(b, placeholderA, N);
    int32_t p = slate_dag_param(b, 0);
    int32_t root = slate_dag_mul(b, slate_dag_load(b, cidA, p), slate_dag_lit(b, 2));
    uint32_t holes[1] = {cidA};
    int rc = slate_dag_save_fragment(b, root, holes, 1, mem_sink, &fragF);
    CHECK(rc == SLATE_BATCH_OK, "save fragment F (A*2)");
    slate_dag_free(b);
  }
  CHECK(fragF.len > 0, "F serialized nonempty");

  /* sanity: splice F alone, run, check A*2 and exact==1 (baseline seam) */
  {
    MemSrc src = {fragF.buf, fragF.len, 0};
    SlateFrag *f = slate_frag_load(mem_src, &src);
    CHECK(f != NULL, "load F");
    SlateDag *b = slate_dag_new();
    uint32_t cidA = slate_dag_carrier(b, A, N);
    uint32_t args[1] = {cidA};
    int32_t root = slate_dag_splice(b, f, args, 1);
    CHECK(root >= 0, "splice F alone");
    int64_t dims[1] = {N};
    SlateArray *a = slate_dag_run(b, root, dims, 1);
    CHECK(a != NULL, "run F alone");
    slate_reading r; memset(&r, 0, sizeof r);
    CHECK(slate_array_receipt(a, &r) == SLATE_BATCH_OK, "F receipt read");
    CHECK(r.exact == 1 && r.refused == 0, "F receipt exact==1");
    /* The run reading's domain agrees with the static iface: a positional integer result is Z (den==1),
     * derived from the result buffer's representation by slate_array_receipt. */
    CHECK(r.domain == 1 /* ℤ */, "F run-reading domain is Z (agrees with iface)");
    int64_t num[N], den[N];
    CHECK(slate_array_i64_unsafe(a, num, den) == SLATE_BATCH_OK, "F pull i64");
    int ok = 1; for (int i = 0; i < N; i++) ok &= (num[i] == exp_F[i] && den[i] == 1);
    CHECK(ok, "F values == A*2");
    slate_array_free(a);
    slate_dag_free(b);
    slate_frag_free(f);
  }

  /* ---------- Level 2: splice F into a fresh builder, build g = F + B, save g as fragment G ----------
   * G must carry two holes: A (from F, wired through the splice) and B (its own carrier). */
  MemSink fragG = {0};
  uint32_t g_slot_of_A = 0, g_slot_of_B = 0; int have_slots = 0;
  {
    MemSrc src = {fragF.buf, fragF.len, 0};
    SlateFrag *f = slate_frag_load(mem_src, &src);
    CHECK(f != NULL, "load F for G-build");
    SlateDag *b = slate_dag_new();
    int64_t placeholderA[N] = {0}, placeholderB[N] = {0};
    uint32_t cidA = slate_dag_carrier(b, placeholderA, N);   /* hole A, wired into F */
    uint32_t cidB = slate_dag_carrier(b, placeholderB, N);   /* hole B, my own */
    uint32_t argsF[1] = {cidA};
    int32_t spliced = slate_dag_splice(b, f, argsF, 1);      /* == A*2 in b's id space */
    CHECK(spliced >= 0, "splice F while building G");
    int32_t p = slate_dag_param(b, 0);
    int32_t g = slate_dag_add(b, spliced, slate_dag_load(b, cidB, p));  /* A*2 + B */
    /* re-save the spliced-and-extended graph as a brand new fragment */
    uint32_t holes[2] = {cidA, cidB};
    int rc = slate_dag_save_fragment(b, g, holes, 2, mem_sink, &fragG);
    CHECK(rc == SLATE_BATCH_OK, "re-save spliced result as fragment G");
    slate_dag_free(b);
    slate_frag_free(f);
  }
  CHECK(fragG.len > 0, "G serialized nonempty");

  /* introspect G: it must report 2 holes and the root PSDA type; learn the hole->carrier order */
  {
    MemSrc src = {fragG.buf, fragG.len, 0};
    SlateFrag *f = slate_frag_load(mem_src, &src);
    CHECK(f != NULL, "load G");
    uint32_t np = 0, nh = 0;
    slate_iface_receipt root_r; memset(&root_r, 0, sizeof root_r);
    int rc = slate_frag_iface(f, &np, NULL, &nh, &root_r);
    CHECK(rc == SLATE_BATCH_OK, "G iface ok");
    CHECK(nh == 2, "G reports 2 holes (A and B)");
    CHECK(root_r.domain == -1 /* derive/any: save_fragment serializes the root receipt domain as derive */,
          "G root iface domain is derive");
    slate_hole holes[2]; uint32_t cap = 2;
    rc = slate_frag_iface(f, NULL, holes, &cap, NULL);
    CHECK(rc == SLATE_BATCH_OK, "G iface fill holes");
    /* Both holes are Z arrays. slot field tells us positional order the fragment expects.
     * The holes[] I passed to save_fragment were {cidA, cidB} in that order; the iface should
     * preserve that order so args[0]->A, args[1]->B. Record whatever slots it reports. */
    for (uint32_t i = 0; i < nh; i++) {
      CHECK(holes[i].receipt.kind == 1 /* array/carrier */, "G hole is array kind");
      CHECK(holes[i].receipt.domain == 1 /* ℤ */, "G hole domain Z");
    }
    g_slot_of_A = holes[0].slot;  /* first-declared hole was A */
    g_slot_of_B = holes[1].slot;  /* second-declared hole was B */
    have_slots = 1;
    (void)g_slot_of_A; (void)g_slot_of_B;
    slate_frag_free(f);
  }
  CHECK(have_slots, "G hole slots recorded");

  /* ---------- Level 3: load G, splice it, final = G + C, run ---------- */
  {
    MemSrc src = {fragG.buf, fragG.len, 0};
    SlateFrag *f = slate_frag_load(mem_src, &src);
    CHECK(f != NULL, "load G for final");
    SlateDag *b = slate_dag_new();
    uint32_t cidA = slate_dag_carrier(b, A, N);
    uint32_t cidB = slate_dag_carrier(b, B, N);
    uint32_t cidC = slate_dag_carrier(b, C, N);
    /* args in iface order: position i supplies the i-th reported hole. First reported == A, second == B. */
    uint32_t args[2] = {cidA, cidB};
    int32_t spliced = slate_dag_splice(b, f, args, 2);       /* == A*2 + B */
    CHECK(spliced >= 0, "splice G (fragment-of-a-fragment)");
    int32_t p = slate_dag_param(b, 0);
    int32_t final = slate_dag_add(b, spliced, slate_dag_load(b, cidC, p));  /* A*2 + B + C */
    int64_t dims[1] = {N};
    SlateArray *a = slate_dag_run(b, final, dims, 1);
    CHECK(a != NULL, "run final (3 levels deep)");
    slate_reading r; memset(&r, 0, sizeof r);
    CHECK(slate_array_receipt(a, &r) == SLATE_BATCH_OK, "final receipt read");
    CHECK(r.exact == 1 && r.refused == 0, "final receipt exact==1 across both seams");
    CHECK(r.domain == 1 /* ℤ */, "final run-reading domain is Z (agrees with iface)");
    CHECK((uint64_t)slate_array_size(a) == (uint64_t)N, "final size == N");
    int64_t num[N], den[N];
    CHECK(slate_array_i64_unsafe(a, num, den) == SLATE_BATCH_OK, "final pull i64");
    printf("  final:");
    for (int i = 0; i < N; i++) printf(" %lld/%lld", (long long)num[i], (long long)den[i]);
    printf("   (want 112 224 336 448)\n");
    int ok = 1; for (int i = 0; i < N; i++) ok &= (num[i] == exp_final[i] && den[i] == 1);
    CHECK(ok, "final values == A*2 + B + C");

    /* Also confirm the intermediate G-level value is what we think (A*2+B) by splicing G alone. */
    slate_dag_free(b);
    SlateDag *b2 = slate_dag_new();
    uint32_t a2 = slate_dag_carrier(b2, A, N);
    uint32_t b2c = slate_dag_carrier(b2, B, N);
    uint32_t args2[2] = {a2, b2c};
    int32_t root2 = slate_dag_splice(b2, f, args2, 2);
    CHECK(root2 >= 0, "splice G alone");
    SlateArray *ga = slate_dag_run(b2, root2, dims, 1);
    CHECK(ga != NULL, "run G alone");
    slate_reading gr; memset(&gr, 0, sizeof gr);
    slate_array_receipt(ga, &gr);
    CHECK(gr.exact == 1, "G-alone receipt exact==1");
    int64_t gn[N], gd[N];
    slate_array_i64_unsafe(ga, gn, gd);
    int gok = 1; for (int i = 0; i < N; i++) gok &= (gn[i] == exp_G[i] && gd[i] == 1);
    CHECK(gok, "G-alone values == A*2 + B");
    slate_array_free(ga);
    slate_dag_free(b2);
    slate_frag_free(f);
  }

  free(fragF.buf);
  free(fragG.buf);
  if (fails == 0) printf("ALL PASS deep_compose\n");
  else printf("%d FAILURE(S) deep_compose\n", fails);
  return fails ? 1 : 0;
}
