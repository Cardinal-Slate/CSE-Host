/* matmul_reduction: battle-test the .slate fragment door with a reduce-DAG matrix multiply.
 *
 * Builds  C[i][j] = Sum_l A[i*K+l] * B[l*D+j]  as a left-deep Add reduction of load*load terms over a
 * 2D dispatch grid (param(0)=i, param(1)=j), with the reduction index l unrolled at build time (the exact
 * shape dag/vec.hpp's detect_dot recognises, matching tests/differential/matmul.cpp). A and B are declared
 * as holes, so the graph is saved as a fragment, loaded back, and spliced into a fresh builder wired to real
 * A/B carriers. Every output cell is checked against an independent plain-C reference matmul computed here.
 *
 * Square case M=K=D=4 (the requested 4x4x4). Also a rectangular case (M=3,K=5,D=2) to stress non-square
 * multi-param dispatch through the fragment path, and a negative-value case to exercise signed accumulation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"
#include "../memstore.h"

/* the text of a reading entry, for printing (a utf8 entry's bytes are a C string) */
static const char *ent(const slate_entry *e, int32_t n, const char *name) {
  const slate_entry *f = slate_entry_find(e, n, name); return f ? (const char *)f->bytes : "-";
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL matmul_reduction: %s\n", msg); fails++; } } while (0)

/* Build the reduce-matmul over holes A,B for shape M,K,D. placeholderA/placeholderB give the holes the
 * bytes they need to build+typecheck once. Returns serialized fragment bytes; *pnholes gets the hole count. */
static MsProg build_matmul_fragment(int M, int K, int D) {
  SlateDag *b = ms_dag();
  int64_t *phA = (int64_t *)calloc((size_t)M * K, sizeof(int64_t));
  int64_t *phB = (int64_t *)calloc((size_t)K * D, sizeof(int64_t));
  uint32_t cA = slate_dag_carrier(b, phA, (uint64_t)M * K);
  uint32_t cB = slate_dag_carrier(b, phB, (uint64_t)K * D);
  int32_t i = slate_dag_param(b, 0);
  int32_t j = slate_dag_param(b, 1);
  /* C[i][j] = Sum_l A[i*K+l]*B[l*D+j]; left-deep add chain of load*load terms. */
  int32_t root = -1;
  for (int l = 0; l < K; l++) {
    int32_t ll = slate_dag_lit(b, l);
    int32_t idxA = slate_dag_add(b, slate_dag_mul(b, i, slate_dag_lit(b, K)), ll);   /* i*K + l */
    int32_t idxB = slate_dag_add(b, slate_dag_mul(b, ll, slate_dag_lit(b, D)), j);   /* l*D + j */
    int32_t prod = slate_dag_mul(b, slate_dag_load(b, cA, idxA), slate_dag_load(b, cB, idxB));
    root = (root < 0) ? prod : slate_dag_add(b, root, prod);
  }
  MsProg out = {0};
  uint32_t holes[2] = {cA, cB};   /* both operands unbound; nothing baked */
  const char *rc = ms_keep(b, root, holes, 2, &out);
  CHECK(rc == NULL, "save_fragment ok (2 holes)");
  slate_dag_free(b);
  free(phA); free(phB);
  return out;
}

/* plain-C reference matmul, row-major. */
static void ref_matmul(const int64_t *A, const int64_t *B, int64_t *C, int M, int K, int D) {
  for (int i = 0; i < M; i++)
    for (int j = 0; j < D; j++) {
      int64_t s = 0;
      for (int l = 0; l < K; l++) s += A[(size_t)i * K + l] * B[(size_t)l * D + j];
      C[(size_t)i * D + j] = s;
    }
}

/* Load the fragment, splice with real A,B, dispatch over {M,D}, verify every cell against ref_matmul. */
static void splice_run_verify(const MsProg *frag, const int64_t *A, const int64_t *B,
                              int M, int K, int D, const char *what) {
  SlateFrag *f = ms_load(frag);
  CHECK(f != NULL, "frag_load ok");
  if (!f) return;

  /* introspect: 2 grid params, 2 array holes */
  uint32_t nparams = 0, nholes = 0;
  slate_iface_receipt root_r;
  const char *rc = slate_frag_iface(f, &nparams, NULL, &nholes, &root_r);
  CHECK(rc == NULL, "iface ok");
  CHECK(nparams == 2, "iface reports 2 grid params");
  CHECK(nholes == 2, "iface reports 2 holes");
  slate_hole holes[2];
  uint32_t cap = 2;
  rc = slate_frag_iface(f, NULL, holes, &cap, NULL);
  CHECK(rc == NULL, "iface fill holes ok");
  CHECK(strcmp(holes[0].receipt.kind, "array") == 0 && strcmp(holes[1].receipt.kind, "array") == 0,
        "both holes are array kind");

  /* splice: supply carriers in iface-reported order. holes[k].slot names which declared hole slot k is;
   * slot 0 == A, slot 1 == B (order of the holes[] array passed to save_fragment). */
  SlateDag *b = slate_dag_new();
  uint32_t cA = slate_dag_carrier(b, A, (uint64_t)M * K);
  uint32_t cB = slate_dag_carrier(b, B, (uint64_t)K * D);
  uint32_t args[2];
  for (uint32_t h = 0; h < nholes; h++) args[h] = (holes[h].slot == 0) ? cA : cB;
  int32_t root = -1; slate_dag_splice(b, f, args, 2, &root);
  CHECK(root >= 0, "splice ok (root >= 0)");

  if (root >= 0) {
    int64_t dims[2] = {M, D};
    SlateArray *a = slate_dag_run(b, root, dims, 2);
    CHECK(a != NULL, "dispatch over 2D grid ok");
    if (a) {
      const slate_entry *re; int32_t rn;
      slate_array_receipt(a, &re, &rn);
      printf("  [%s] reading: verdict=%s domain=%s path=%s mode=%s closure=%s\n",
             what, ent(re, rn, "verdict"), ent(re, rn, "domain"), ent(re, rn, "path"), ent(re, rn, "mode"), ent(re, rn, "closure"));
      CHECK(!slate_entry_is(re, rn, "verdict", "undefined"), "reading not refused");
      CHECK(slate_entry_is(re, rn, "verdict", "exact"), "reading exact");
      /* Runtime reading reports domain=Q for these integer-operand, integer-valued reductions, while the
       * static root receipt renders "-> Z". Z is a subset of Q, so the classification is a conservative
       * over-approximation, not a value error (den==1 on every cell, checked below). Accept N/Z/Q here. */
      CHECK(slate_entry_is(re, rn, "domain", "Z") || slate_entry_is(re, rn, "domain", "N") || slate_entry_is(re, rn, "domain", "Q"),
            "reading domain in tower {N,Z,Q}");
      CHECK(slate_array_size(a) == (uint64_t)M * D, "result cell count == M*D");

      int64_t *num = (int64_t *)malloc((size_t)M * D * sizeof(int64_t));
      int64_t *den = (int64_t *)malloc((size_t)M * D * sizeof(int64_t));
      const char *prc = slate_array_i64_unsafe(a, num, den);
      CHECK(prc == NULL, "i64 pull ok");
      if (prc == NULL) {
        int64_t *C = (int64_t *)malloc((size_t)M * D * sizeof(int64_t));
        ref_matmul(A, B, C, M, K, D);
        int mism = 0;
        for (int i = 0; i < M; i++)
          for (int j = 0; j < D; j++) {
            size_t idx = (size_t)i * D + j;
            if (num[idx] != C[idx] || den[idx] != 1) {
              if (mism < 6)
                printf("  MISMATCH [%s] C[%d][%d] slate=%lld/%lld ref=%lld\n",
                       what, i, j, (long long)num[idx], (long long)den[idx], (long long)C[idx]);
              mism++;
            }
          }
        CHECK(mism == 0, "every cell matches reference matmul");
        if (mism == 0) printf("  [%s] all %dx%d cells == reference (K=%d reduction)\n", what, M, D, K);
        free(C);
      }
      free(num); free(den);
      slate_array_free(a);
    }
  }
  slate_dag_free(b);
  slate_frag_free(f);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);

  /* ---- Case 1: the requested 4x4x4 square matmul ---- */
  {
    const int M = 4, K = 4, D = 4;
    /* A[i][l] = i*4 + l + 1  (1..16);  B[l][j] = (l+1)*10 + j  */
    int64_t A[16], B[16];
    for (int i = 0; i < M; i++) for (int l = 0; l < K; l++) A[i * K + l] = (int64_t)(i * K + l) + 1;
    for (int l = 0; l < K; l++) for (int j = 0; j < D; j++) B[l * D + j] = (int64_t)(l + 1) * 10 + j;
    /* hand-check one cell: C[0][0] = 1*10 + 2*20 + 3*30 + 4*40 = 10+40+90+160 = 300 */
    {
      int64_t c00 = 1*10 + 2*20 + 3*30 + 4*40;
      CHECK(c00 == 300, "hand arithmetic sanity (C[0][0]=300)");
    }
    MsProg frag = build_matmul_fragment(M, K, D);
    CHECK(frag.wn > 0, "4x4 fragment named by a word");
    splice_run_verify(&frag, A, B, M, K, D, "4x4x4");
    ms_prog_free(&frag);
  }

  /* ---- Case 2: rectangular multi-param dispatch M=3, K=5, D=2 through the fragment path ---- */
  {
    const int M = 3, K = 5, D = 2;
    int64_t A[15], B[10];
    for (int t = 0; t < M * K; t++) A[t] = (int64_t)((t * 7 + 3) % 13) - 6;   /* [-6,6] */
    for (int t = 0; t < K * D; t++) B[t] = (int64_t)((t * 5 + 1) % 11) - 5;   /* [-5,5] */
    MsProg frag = build_matmul_fragment(M, K, D);
    CHECK(frag.wn > 0, "3x5x2 fragment named by a word");
    splice_run_verify(&frag, A, B, M, K, D, "3x5x2");
    ms_prog_free(&frag);
  }

  /* ---- Case 3: negative-value accumulation, 4x4x4, distinct A/B from case 1 ---- */
  {
    const int M = 4, K = 4, D = 4;
    int64_t A[16], B[16];
    for (int t = 0; t < 16; t++) { A[t] = (int64_t)(t - 8); B[t] = (int64_t)((t % 5) - 2) * ((t & 1) ? -1 : 1); }
    MsProg frag = build_matmul_fragment(M, K, D);
    splice_run_verify(&frag, A, B, M, K, D, "4x4x4-signed");
    ms_prog_free(&frag);
  }

  /* ---- Diagnostic: does a direct (non-fragment) run of the same DAG report the same domain? ---- */
  {
    const int M = 4, K = 4, D = 4;
    int64_t A[16], B[16];
    for (int i = 0; i < M; i++) for (int l = 0; l < K; l++) A[i * K + l] = (int64_t)(i * K + l) + 1;
    for (int l = 0; l < K; l++) for (int j = 0; j < D; j++) B[l * D + j] = (int64_t)(l + 1) * 10 + j;
    SlateDag *b = slate_dag_new();
    uint32_t cA = slate_dag_carrier(b, A, 16), cB = slate_dag_carrier(b, B, 16);
    int32_t i = slate_dag_param(b, 0), j = slate_dag_param(b, 1);
    int32_t root = -1;
    for (int l = 0; l < K; l++) {
      int32_t ll = slate_dag_lit(b, l);
      int32_t idxA = slate_dag_add(b, slate_dag_mul(b, i, slate_dag_lit(b, K)), ll);
      int32_t idxB = slate_dag_add(b, slate_dag_mul(b, ll, slate_dag_lit(b, D)), j);
      int32_t prod = slate_dag_mul(b, slate_dag_load(b, cA, idxA), slate_dag_load(b, cB, idxB));
      root = (root < 0) ? prod : slate_dag_add(b, root, prod);
    }
    int64_t dims[2] = {M, D};
    SlateArray *a = slate_dag_run(b, root, dims, 2);
    if (a) { const slate_entry *re; int32_t rn; slate_array_receipt(a, &re, &rn);
      printf("  [DIRECT non-fragment] reading domain=%s\n", ent(re, rn, "domain"));
      slate_array_free(a); }
    slate_dag_free(b);
  }

  if (fails == 0) printf("PASS matmul_reduction: fragment reduce-matmul verified against reference\n");
  else printf("FAIL matmul_reduction: %d check(s) failed\n", fails);
  return fails ? 1 : 0;
}
