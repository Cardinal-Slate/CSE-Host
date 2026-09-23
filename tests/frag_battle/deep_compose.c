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
#include "slate/slate.h"
#include "../memstore.h"


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
  MsProg fragF = {0};
  {
    SlateDag *b = ms_dag();
    int64_t placeholderA[N] = {0};
    uint32_t cidA = slate_dag_carrier(b, placeholderA, N);
    int32_t p = slate_dag_param(b, 0);
    int32_t root = slate_dag_mul(b, slate_dag_load(b, cidA, p), slate_dag_lit(b, 2));
    uint32_t holes[1] = {cidA};
    const char *rc = ms_keep(b, root, holes, 1, &fragF);
    CHECK(rc == NULL, "save fragment F (A*2)");
    slate_dag_free(b);
  }
  CHECK(fragF.pn > 0, "F serialized nonempty");

  /* sanity: splice F alone, run, check A*2 and exact==1 (baseline seam) */
  {
    SlateFrag *f = ms_load(&fragF);
    CHECK(f != NULL, "load F");
    SlateDag *b = slate_dag_new();
    uint32_t cidA = slate_dag_carrier(b, A, N);
    uint32_t args[1] = {cidA};
    int32_t root = -1; slate_dag_splice(b, f, args, 1, &root);
    CHECK(root >= 0, "splice F alone");
    int64_t dims[1] = {N};
    SlateArray *a = slate_dag_run(b, root, dims, 1);
    CHECK(a != NULL, "run F alone");
    const slate_entry *re = NULL; int32_t rn = 0;
    CHECK(slate_array_receipt(a, &re, &rn) == NULL, "F receipt read");
    CHECK(slate_entry_is(re, rn, "verdict", "exact"), "F receipt exact");
    /* The run reading's domain agrees with the static iface: a positional integer result is Z (den==1),
     * derived from the result buffer's representation by slate_array_receipt. */
    CHECK(slate_entry_is(re, rn, "domain", "Z"), "F run-reading domain is Z (agrees with iface)");
    int64_t num[N], den[N];
    CHECK(slate_array_i64_unsafe(a, num, den) == NULL, "F pull i64");
    int ok = 1; for (int i = 0; i < N; i++) ok &= (num[i] == exp_F[i] && den[i] == 1);
    CHECK(ok, "F values == A*2");
    slate_array_free(a);
    slate_dag_free(b);
    slate_frag_free(f);
  }

  /* ---------- Level 2: splice F into a fresh builder, build g = F + B, save g as fragment G ----------
   * G must carry two holes: A (from F, wired through the splice) and B (its own carrier). */
  MsProg fragG = {0};
  uint32_t g_slot_of_A = 0, g_slot_of_B = 0; int have_slots = 0;
  {
    SlateFrag *f = ms_load(&fragF);
    CHECK(f != NULL, "load F for G-build");
    SlateDag *b = ms_dag();
    int64_t placeholderA[N] = {0}, placeholderB[N] = {0};
    uint32_t cidA = slate_dag_carrier(b, placeholderA, N);   /* hole A, wired into F */
    uint32_t cidB = slate_dag_carrier(b, placeholderB, N);   /* hole B, my own */
    uint32_t argsF[1] = {cidA};
    int32_t spliced = -1; slate_dag_splice(b, f, argsF, 1, &spliced);      /* == A*2 in b's id space */
    CHECK(spliced >= 0, "splice F while building G");
    int32_t p = slate_dag_param(b, 0);
    int32_t g = slate_dag_add(b, spliced, slate_dag_load(b, cidB, p));  /* A*2 + B */
    /* re-save the spliced-and-extended graph as a brand new fragment */
    uint32_t holes[2] = {cidA, cidB};
    const char *rc = ms_keep(b, g, holes, 2, &fragG);
    CHECK(rc == NULL, "re-save spliced result as fragment G");
    slate_dag_free(b);
    slate_frag_free(f);
  }
  CHECK(fragG.pn > 0, "G serialized nonempty");

  /* introspect G: it must report 2 holes and the root PSDA type; learn the hole->carrier order */
  {
    SlateFrag *f = ms_load(&fragG);
    CHECK(f != NULL, "load G");
    uint32_t np = 0, nh = 0;
    slate_iface_receipt root_r; memset(&root_r, 0, sizeof root_r);
    const char *rc = slate_frag_iface(f, &np, NULL, &nh, &root_r);
    CHECK(rc == NULL, "G iface ok");
    CHECK(nh == 2, "G reports 2 holes (A and B)");
    CHECK(root_r.domain == NULL /* derive/any: save_fragment serializes the root receipt domain as derive */,
          "G root iface domain is derive");
    slate_hole holes[2]; uint32_t cap = 2;
    rc = slate_frag_iface(f, NULL, holes, &cap, NULL);
    CHECK(rc == NULL, "G iface fill holes");
    /* Both holes are Z arrays. slot field tells us positional order the fragment expects.
     * The holes[] I passed to save_fragment were {cidA, cidB} in that order; the iface should
     * preserve that order so args[0]->A, args[1]->B. Record whatever slots it reports. */
    for (uint32_t i = 0; i < nh; i++) {
      CHECK(strcmp(holes[i].receipt.kind, "array") == 0, "G hole is array kind");
      CHECK(strcmp(holes[i].receipt.domain, "Z") == 0, "G hole domain Z");
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
    SlateFrag *f = ms_load(&fragG);
    CHECK(f != NULL, "load G for final");
    SlateDag *b = slate_dag_new();
    uint32_t cidA = slate_dag_carrier(b, A, N);
    uint32_t cidB = slate_dag_carrier(b, B, N);
    uint32_t cidC = slate_dag_carrier(b, C, N);
    /* args in iface order: position i supplies the i-th reported hole. First reported == A, second == B. */
    uint32_t args[2] = {cidA, cidB};
    int32_t spliced = -1; slate_dag_splice(b, f, args, 2, &spliced);       /* == A*2 + B */
    CHECK(spliced >= 0, "splice G (fragment-of-a-fragment)");
    int32_t p = slate_dag_param(b, 0);
    int32_t final = slate_dag_add(b, spliced, slate_dag_load(b, cidC, p));  /* A*2 + B + C */
    int64_t dims[1] = {N};
    SlateArray *a = slate_dag_run(b, final, dims, 1);
    CHECK(a != NULL, "run final (3 levels deep)");
    const slate_entry *re = NULL; int32_t rn = 0;
    CHECK(slate_array_receipt(a, &re, &rn) == NULL, "final receipt read");
    CHECK(slate_entry_is(re, rn, "verdict", "exact"), "final receipt exact across both seams");
    CHECK(slate_entry_is(re, rn, "domain", "Z"), "final run-reading domain is Z (agrees with iface)");
    CHECK((uint64_t)slate_array_size(a) == (uint64_t)N, "final size == N");
    int64_t num[N], den[N];
    CHECK(slate_array_i64_unsafe(a, num, den) == NULL, "final pull i64");
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
    int32_t root2 = -1; slate_dag_splice(b2, f, args2, 2, &root2);
    CHECK(root2 >= 0, "splice G alone");
    SlateArray *ga = slate_dag_run(b2, root2, dims, 1);
    CHECK(ga != NULL, "run G alone");
    const slate_entry *ge = NULL; int32_t gn_ = 0;
    slate_array_receipt(ga, &ge, &gn_);
    CHECK(slate_entry_is(ge, gn_, "verdict", "exact"), "G-alone receipt exact");
    int64_t gn[N], gd[N];
    slate_array_i64_unsafe(ga, gn, gd);
    int gok = 1; for (int i = 0; i < N; i++) gok &= (gn[i] == exp_G[i] && gd[i] == 1);
    CHECK(gok, "G-alone values == A*2 + B");
    slate_array_free(ga);
    slate_dag_free(b2);
    slate_frag_free(f);
  }

  ms_prog_free(&fragF);
  ms_prog_free(&fragG);
  if (fails == 0) printf("ALL PASS deep_compose\n");
  else printf("%d FAILURE(S) deep_compose\n", fails);
  return fails ? 1 : 0;
}
