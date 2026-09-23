/* identity_map: the trivial fragment — root = load(A, param0), one hole A, zero arithmetic ops.
 *
 * A pure passthrough: the body is a single load of hole A at the grid param, so splicing and running it
 * over grid[n] reproduces A cell-for-cell with no transform. Edge focus: no arithmetic node in the graph
 * at all; negative and zero cells must round-trip.
 *
 * Every numeric assertion is checked against a value computed by hand (the input array itself). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"
#include "../memstore.h"

/* growable memory sink + cursor source over the same bytes (the frag_cabi pattern) */

static int fails = 0;
#define CHECK(cond, msg) do { \
  if (cond) { printf("PASS: %s\n", msg); } \
  else { printf("FAIL: %s\n", msg); fails++; } } while (0)

#define N 4

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);

  /* ---- Build: root = load(A, param0). No add/mul/anything. A is the sole hole. ---- */
  MsProg frag = {0};
  {
    SlateDag *b = ms_dag();
    CHECK(b != NULL, "slate_dag_new builder allocated");
    int64_t placeholderA[N] = {0, 0, 0, 0};          /* a hole still needs data to typecheck once */
    uint32_t cidA = slate_dag_carrier(b, placeholderA, N);
    CHECK(cidA != UINT32_MAX, "carrier registered (hole A)");
    int32_t p = slate_dag_param(b, 0);
    int32_t root = slate_dag_load(b, cidA, p);        /* the entire graph: one load, zero ops */
    CHECK(root >= 0, "load node id valid (root is a bare load)");
    uint32_t holes[1] = {cidA};
    const char *rc = ms_keep(b, root, holes, 1, &frag);
    CHECK(rc == NULL, "save_fragment ok (zero-op fragment)");
    slate_dag_free(b);
  }
  CHECK(frag.pn > 0, "fragment serialized nonempty");

  /* ---- Load + iface: must report exactly 1 param and 1 hole ---- */
  {
    SlateFrag *f = ms_load(&frag);
    CHECK(f != NULL, "frag_load ok");
    if (f) {
      uint32_t np = 999, nh = 999;
      slate_iface_receipt root_r;
      const char *rc = slate_frag_iface(f, &np, NULL, &nh, &root_r);
      CHECK(rc == NULL, "iface ok");
      CHECK(np == 1, "iface reports 1 param");
      CHECK(nh == 1, "iface reports 1 hole");
      /* fill the hole descriptor: slot 0, array kind */
      slate_hole holes[1];
      uint32_t cap = 1;
      rc = slate_frag_iface(f, NULL, holes, &cap, NULL);
      CHECK(rc == NULL, "iface fill ok");
      CHECK(holes[0].slot == 0, "hole is slot 0");
      CHECK(strcmp(holes[0].receipt.kind, "array") == 0, "hole is array kind");
      slate_frag_free(f);
    }
  }

  /* ---- Splice + run: wire real A = {5,-3,7,0} into the hole, run over grid[4] ---- */
  {
    const int64_t A[N] = {5, -3, 7, 0};               /* negative and zero cells on purpose */
    SlateFrag *f = ms_load(&frag);
    CHECK(f != NULL, "frag_load ok (for splice)");
    if (f) {
      SlateDag *b = slate_dag_new();
      uint32_t cidA = slate_dag_carrier(b, A, N);
      CHECK(cidA != UINT32_MAX, "real A carrier registered");
      uint32_t args[1] = {cidA};
      int32_t root = -1; slate_dag_splice(b, f, args, 1, &root);
      CHECK(root >= 0, "splice ok (root node returned)");
      if (root >= 0) {
        int64_t dims[1] = {N};
        SlateArray *a = slate_dag_run(b, root, dims, 1);
        CHECK(a != NULL, "run ok (zero-op dispatch)");
        if (a) {
          /* receipt: exact, not refused, Z domain (integer passthrough) */
          const slate_entry *re; int32_t rn;
          const char *rrc = slate_array_receipt(a, &re, &rn);
          CHECK(rrc == NULL, "receipt read ok");
          CHECK(slate_entry_is(re, rn, "verdict", "exact"), "reading is exact");
          CHECK(!slate_entry_is(re, rn, "verdict", "undefined"), "reading is not refused");
          CHECK(slate_array_size(a) == (uint64_t)N, "result has N cells");

          int64_t num[N], den[N];
          const char *erc = slate_array_i64_unsafe(a, num, den);
          CHECK(erc == NULL, "i64 extract ok");
          /* hand-computed: identity means out == A exactly, den == 1 */
          CHECK(num[0] == 5  && den[0] == 1, "cell0 == 5   (positive)");
          CHECK(num[1] == -3 && den[1] == 1, "cell1 == -3  (negative round-trips)");
          CHECK(num[2] == 7  && den[2] == 1, "cell2 == 7   (positive)");
          CHECK(num[3] == 0  && den[3] == 1, "cell3 == 0   (zero round-trips)");

          int all = 1;
          for (int i = 0; i < N; i++) if (num[i] != A[i] || den[i] != 1) all = 0;
          CHECK(all, "out == A cell-for-cell (identity)");
          slate_array_free(a);
        }
      }
      slate_dag_free(b);
      slate_frag_free(f);
    }
  }

  if (fails == 0) printf("\nALL PASS: identity_map (%d checks)\n", 0);
  else printf("\n%d FAILURE(S): identity_map\n", fails);
  return fails ? 1 : 0;
}
