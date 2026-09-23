/* The fragment-library door from pure C (slate/slate.h). No C++ in this translation unit. Builds a fragment
 * with one hole (A) and one baked constant carrier (C), computing root[i] = A[i]*2 + C[i] over grid[i]; keeps
 * it as a leaf through the store (one cell per byte, under its own word), walks it back by that word — cell 0,
 * cell 1, … to the first cell nobody has, because nothing anywhere says how many there are — checks the
 * interface, splices it into two fresh builders with different A data, runs, and verifies the exact cells.
 * Then it exercises robustness (an unkept word, an empty word, a store that hands back a different byte, a
 * store that lost a cell so the walk stops early — every one refuses), the size-then-fill iface path, splice
 * typecheck refusals, and the mulh/asr prims.
 *
 * A program is a leaf, so a fragment only exists where a store kept it: every builder here is handed the one
 * table in `memstore.h`, which is also how one container loads what another kept. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"
#include "../../memstore.h"

/* a program's name, as the doors hand it back: its word, and nothing beside it */
typedef struct { uint8_t *w; uint64_t wn; } Prog;
static void prog_free(Prog *p) { free(p->w); p->w = NULL; p->wn = 0; }

static MemStore store;
static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL frag_cabi: %s\n", msg); fails++; } } while (0)

/* build root[i] = A[i]*2 + C[i], A a hole, C baked = {10,20,30,40}; return the program's name. */
static Prog build_and_save(void) {
  SlateDag *b = slate_dag_new();
  MS_INSTALL(b, &store);
  int64_t placeholderA[4] = {0, 0, 0, 0};            /* the hole needs data to build+typecheck once */
  int64_t bakedC[4] = {10, 20, 30, 40};
  uint32_t cidA = slate_dag_carrier(b, placeholderA, 4);
  uint32_t cidC = slate_dag_carrier(b, bakedC, 4);
  int32_t p = slate_dag_param(b, 0);
  int32_t lA = slate_dag_load(b, cidA, p);
  int32_t lC = slate_dag_load(b, cidC, p);
  int32_t m = slate_dag_mul(b, lA, slate_dag_lit(b, 2));
  int32_t root = slate_dag_add(b, m, lC);
  Prog out = {0};
  uint32_t holes[1] = {cidA};
  const char *rc = slate_dag_save_fragment(b, root, holes, 1, &out.w, &out.wn);
  CHECK(rc == NULL, "save_fragment ok");
  slate_dag_free(b);
  return out;
}

/* walk the program back by its word into a fresh container over the same store, splice it over A, run on
   grid[4], and read the 4 int64 cells into out[4]. */
static int splice_run(const Prog *frag, const int64_t A[4], int64_t out[4]) {
  SlateDag *b = slate_dag_new();
  MS_INSTALL(b, &store);
  SlateFrag *f = slate_frag_load(b, frag->w, frag->wn);
  if (!f) { slate_dag_free(b); return -1000; }
  uint32_t cidA = slate_dag_carrier(b, A, 4);
  uint32_t args[1] = {cidA};
  int32_t root = -1; slate_dag_splice(b, f, args, 1, &root);
  int rc = -2000;
  if (root >= 0) {
    int64_t dims[1] = {4};
    SlateArray *a = slate_dag_run(b, root, dims, 1);
    if (a) {
      int64_t num[4], den[4];
      if (slate_array_i64_unsafe(a, num, den) == NULL) {
        for (int i = 0; i < 4; i++) out[i] = num[i];    /* den == 1 for these integer cells */
        rc = 0;
      }
      slate_array_free(a);
    }
  } else rc = root;
  slate_dag_free(b);
  slate_frag_free(f);
  return rc;
}

/* a container over the one store, for a load that is expected to refuse */
static SlateFrag *load_by(const uint8_t *w, uint64_t wn, SlateDag **keep) {
  SlateDag *b = slate_dag_new();
  MS_INSTALL(b, &store);
  SlateFrag *f = slate_frag_load(b, w, wn);
  if (keep) *keep = b; else slate_dag_free(b);
  return f;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ms_init(&store, 1 << 14);

  /* 1. round-trip + splice + run */
  Prog frag = build_and_save();
  CHECK(frag.wn > 0, "fragment kept as a leaf, named by its word and nothing else");
  CHECK(store.rows > 0 && store.rows == ms_leaf_len(&store), "the leaf is one row per byte, and that is its length");

  /* 2. iface */
  {
    SlateDag *hb = NULL;
    SlateFrag *f = load_by(frag.w, frag.wn, &hb);
    CHECK(f != NULL, "frag_load ok");
    if (f) {
      uint32_t np = 0, nh = 0;
      slate_iface_receipt root_r;
      const char *rc = slate_frag_iface(f, &np, NULL, &nh, &root_r);
      CHECK(rc == NULL, "iface ok");
      CHECK(np == 1, "iface nparams == 1");
      CHECK(nh == 1, "iface nholes == 1");
      /* size-then-fill: undersized buffer returns EOUTSIZE */
      slate_hole holes[1];
      uint32_t cap0 = 0;
      CHECK(slate_refused(slate_frag_iface(f, NULL, holes, &cap0, NULL), "outsize"), "iface EOUTSIZE on 0 cap");
      uint32_t cap1 = 1;
      rc = slate_frag_iface(f, NULL, holes, &cap1, NULL);
      CHECK(rc == NULL && holes[0].slot == 0 && strcmp(holes[0].receipt.kind, "array") == 0,
            "iface fills the hole (slot 0, array kind)");
      slate_frag_free(f);
    }
    slate_dag_free(hb);
  }

  /* 3. splice+run twice with different A — one loaded fragment, two builders */
  {
    int64_t A1[4] = {1, 2, 3, 4}, out1[4] = {0};
    int rc = splice_run(&frag, A1, out1);
    CHECK(rc == 0, "splice_run #1 ok");
    /* expect A*2 + C = {1*2+10, 2*2+20, 3*2+30, 4*2+40} = {12,24,36,48} */
    CHECK(out1[0] == 12 && out1[1] == 24 && out1[2] == 36 && out1[3] == 48, "splice_run #1 values");

    int64_t A2[4] = {100, 0, -5, 7}, out2[4] = {0};
    rc = splice_run(&frag, A2, out2);
    CHECK(rc == 0, "splice_run #2 ok");
    CHECK(out2[0] == 210 && out2[1] == 20 && out2[2] == 20 && out2[3] == 54, "splice_run #2 values");
  }

  /* 4. robustness: a word nothing was kept under, and an empty word — both refuse (NULL), no crash. There is
   *    no file to corrupt any more and no count to lie about: all a load is handed is a word, and what can lie
   *    to it is the store it walks. */
  {
    uint8_t *junk = (uint8_t *)malloc((size_t)frag.wn);
    for (uint64_t i = 0; i < frag.wn; i++) junk[i] = (uint8_t)(frag.w[i] ^ 0xA5);
    CHECK(load_by(junk, frag.wn, NULL) == NULL, "a word nothing was kept under refused");
    free(junk);
    CHECK(load_by(frag.w, 0, NULL) == NULL, "an empty word refused");
  }

  /* 4a. a store that hands back a different byte: the crc over the body catches it. One cell of the leaf is
   *    a row like any other, so corrupting the store is corrupting one byte of the program — which is exactly
   *    what the crc is for now that no file carries one. Kept in a table of its own, so every row in it is a
   *    cell of this one leaf and nothing else is disturbed. */
  {
    MemStore only; ms_init(&only, 1 << 12);
    SlateDag *kb = slate_dag_new(); MS_INSTALL(kb, &only);
    int64_t ph[4] = {0, 0, 0, 0}, bc[4] = {10, 20, 30, 40};
    uint32_t ca = slate_dag_carrier(kb, ph, 4), cc = slate_dag_carrier(kb, bc, 4);
    int32_t pp = slate_dag_param(kb, 0);
    int32_t rt = slate_dag_add(kb, slate_dag_mul(kb, slate_dag_load(kb, ca, pp), slate_dag_lit(kb, 2)),
                               slate_dag_load(kb, cc, pp));
    Prog one = {0};
    uint32_t hs[1] = {ca};
    CHECK(slate_dag_save_fragment(kb, rt, hs, 1, &one.w, &one.wn) == NULL, "leaf kept in its own table");
    slate_dag_free(kb);
    CHECK(only.rows > 0 && only.rows == ms_leaf_len(&only), "the table holds exactly one row per program byte");
    int flipped = 0;                               /* every row here is a cell of that leaf: flip one residue */
    for (size_t i = 0; i < only.nbuckets && !flipped; i++)
      for (MsRow *r = only.tab[i]; r; r = r->next)
        if (r->n >= 9) { r->b[1] ^= 0x01; flipped = 1; break; }
    CHECK(flipped, "a cell row was found to corrupt");
    SlateDag *lb = slate_dag_new(); MS_INSTALL(lb, &only);
    SlateFrag *f = slate_frag_load(lb, one.w, one.wn);
    CHECK(f == NULL, "a store that hands back a different byte is refused");
    if (f) slate_frag_free(f);
    slate_dag_free(lb);
    prog_free(&one);
    ms_free(&only);
  }

  /* 4b. a store that lost a cell: the walk stops at the first cell nobody has, so what reaches the parser is
   * short. Nothing said how long the leaf was, so nothing catches this before the parser — and the parser must
   * catch it at every stopping point. A walk that stopped early is never parsed into a fragment, so it is
   * never spliced and never run. Its own table again, so every row in it is a cell of this one leaf. */
  {
    MemStore lost; ms_init(&lost, 1 << 12);
    SlateDag *kb = slate_dag_new(); MS_INSTALL(kb, &lost);
    int64_t ph[4] = {0, 0, 0, 0}, bc[4] = {10, 20, 30, 40};
    uint32_t ca = slate_dag_carrier(kb, ph, 4), cc = slate_dag_carrier(kb, bc, 4);
    int32_t pp = slate_dag_param(kb, 0);
    int32_t rt = slate_dag_add(kb, slate_dag_mul(kb, slate_dag_load(kb, ca, pp), slate_dag_lit(kb, 2)),
                               slate_dag_load(kb, cc, pp));
    Prog one = {0};
    uint32_t hs[1] = {ca};
    CHECK(slate_dag_save_fragment(kb, rt, hs, 1, &one.w, &one.wn) == NULL, "leaf kept for the lost-cell walk");
    slate_dag_free(kb);
    const size_t len = ms_leaf_len(&lost);
    CHECK(len > 0, "the lost-cell walk has a leaf to shorten");
    int ran = 0;
    for (size_t stop = 0; stop < len; stop++) {          /* the walk stops after `stop` cells */
      ms_leaf_stop_after(&lost, stop);
      SlateDag *sb = slate_dag_new(); MS_INSTALL(sb, &lost);
      SlateFrag *sf = slate_frag_load(sb, one.w, one.wn);
      if (sf) { ran++; slate_frag_free(sf); }
      slate_dag_free(sb);
    }
    CHECK(ran == 0, "a walk that stopped early was never parsed into a fragment");
    ms_leaf_stop_after(&lost, len);                      /* every cell back: the whole leaf loads again */
    {
      SlateDag *sb = slate_dag_new(); MS_INSTALL(sb, &lost);
      SlateFrag *sf = slate_frag_load(sb, one.w, one.wn);
      CHECK(sf != NULL, "the whole walk loads once every cell is there again");
      if (sf) slate_frag_free(sf);
      slate_dag_free(sb);
    }
    prog_free(&one);
    ms_free(&lost);
  }

  /* 5. splice typecheck / arg errors: wrong arity and an out-of-range carrier id both refuse cleanly */
  {
    SlateFrag *f = load_by(frag.w, frag.wn, NULL);
    SlateDag *b = slate_dag_new();
    MS_INSTALL(b, &store);
    int64_t A[4] = {1, 2, 3, 4};
    uint32_t cidA = slate_dag_carrier(b, A, 4);
    uint32_t args_ok[1] = {cidA};
    uint32_t args_bad[1] = {9999};                    /* not a registered carrier */
    CHECK(slate_refused(slate_dag_splice(b, f, args_ok, 0, NULL), "args"), "splice wrong arity refused");
    CHECK(slate_refused(slate_dag_splice(b, f, args_bad, 1, NULL), "args"), "splice bad carrier id refused");
    slate_dag_free(b);
    slate_frag_free(f);
  }

  /* 6. the new prims: asr(1000, 2) == 250, and smulh(2^40, 2^40) == 2^16 */
  {
    SlateDag *b = slate_dag_new();
    int32_t r = slate_dag_asr(b, slate_dag_lit(b, 1000), slate_dag_lit(b, 2));
    int64_t dims[1] = {1};
    SlateArray *a = slate_dag_run(b, r, dims, 1);
    CHECK(a != NULL, "asr run ok");
    if (a) { int64_t num[1], den[1]; slate_array_i64_unsafe(a, num, den);
             CHECK(num[0] == 250, "asr(1000,2) == 250"); slate_array_free(a); }
    slate_dag_free(b);

    b = slate_dag_new();
    int64_t big = (int64_t)1 << 40;
    int32_t h = slate_dag_mulh(b, slate_dag_lit(b, big), slate_dag_lit(b, big));
    a = slate_dag_run(b, h, dims, 1);
    CHECK(a != NULL, "mulh run ok");
    if (a) { int64_t num[1], den[1]; slate_array_i64_unsafe(a, num, den);
             CHECK(num[0] == ((int64_t)1 << 16), "smulh(2^40,2^40) == 2^16"); slate_array_free(a); }
    slate_dag_free(b);
  }

  /* 7. a fragment that uses asr round-trips (feature declared, still loads/splices/runs) */
  {
    SlateDag *b = slate_dag_new();
    MS_INSTALL(b, &store);
    int64_t placeholder[4] = {0, 0, 0, 0};
    uint32_t cid = slate_dag_carrier(b, placeholder, 4);
    int32_t p = slate_dag_param(b, 0);
    int32_t l = slate_dag_load(b, cid, p);
    int32_t root = slate_dag_asr(b, l, slate_dag_lit(b, 1));    /* root[i] = A[i] >> 1 */
    Prog out = {0};
    uint32_t holes[1] = {cid};
    CHECK(slate_dag_save_fragment(b, root, holes, 1, &out.w, &out.wn) == NULL, "asr fragment kept");
    slate_dag_free(b);

    int64_t A[4] = {8, 9, 10, 100}, res[4] = {0};
    int rc = splice_run(&out, A, res);
    CHECK(rc == 0, "asr fragment splice_run ok");
    CHECK(res[0] == 4 && res[1] == 4 && res[2] == 5 && res[3] == 50, "asr fragment values");
    prog_free(&out);
  }

  /* 8. token-based repeated reload — the durable handle a caller carries across time, which is a name and
   *    never a bare node id (a node id is a within-build positional handle): keep the fragment once, then
   *    load + splice + run that same name repeatedly (3 cycles) with the same input, and check the value
   *    reproduces identically each cycle. Position-independent: every cycle loads into a fresh builder over
   *    the same store. */
  {
    int64_t A[4] = {7, 11, 13, 17};
    int64_t want[4] = {7 * 2 + 10, 11 * 2 + 20, 13 * 2 + 30, 17 * 2 + 40};   /* A*2 + baked C */
    for (int cycle = 0; cycle < 3; cycle++) {
      int64_t got[4] = {0};
      int rc = splice_run(&frag, A, got);
      CHECK(rc == 0, "token repeated reload: splice_run ok each cycle");
      CHECK(got[0] == want[0] && got[1] == want[1] && got[2] == want[2] && got[3] == want[3],
            "token repeated reload: value reproduces each cycle");
    }
  }

  /* 9. Regression (carrier INT64_MIN soundness): a carrier cell holding INT64_MIN must read back exactly
   *    -2^63 (or refuse) — never a silent 0. Root cause was a signed `-v` magnitude in the carrier packer:
   *    |INT64_MIN| = 2^63 overflows int64 (`-v` stays negative), so the max-magnitude scan left the width at
   *    zero and every cell read back 0. INT64_MIN+1 / INT64_MAX were always fine — the collision is unique to
   *    INT64_MIN, so they serve as controls. Exact-or-refuse: never a silently wrong value. */
  {
    int64_t cases[3] = { INT64_MIN, INT64_MIN + 1, INT64_MAX };
    for (int k = 0; k < 3; k++) {
      SlateDag *b = slate_dag_new();
      int64_t v[1] = { cases[k] };
      uint32_t cid = slate_dag_carrier(b, v, 1);
      int32_t root = slate_dag_load(b, cid, slate_dag_lit(b, 0));
      int64_t dims[1] = {1};
      SlateArray *a = slate_dag_run(b, root, dims, 1);
      CHECK(a != NULL, "carrier INT64_MIN: run produced a result");
      if (a) {
        int64_t num[1], den[1];
        const char *rc = slate_array_i64_unsafe(a, num, den);
        /* fast i64 reader: exact value, or a clean refusal — but never OK-with-0 for a nonzero cell */
        CHECK(!(rc == NULL && num[0] == 0), "carrier value never silently reads back as 0");
        if (rc == NULL)
          CHECK(num[0] == cases[k] && den[0] == 1, "carrier value reads back exactly via i64");
        /* the full sign+magnitude reader must always be exact: sign set for INT64_MIN, magnitude = 2^63 */
        uint64_t stride = 0;
        slate_array_records(a, NULL, 0, &stride);
        uint64_t *rec = (uint64_t *)malloc((size_t)stride * 8);
        const char *rrc = slate_array_records(a, rec, stride * 8, &stride);
        CHECK(rrc == NULL, "records reader ok");
        if (rrc == NULL) {
          uint64_t want_sign = cases[k] < 0 ? 1 : 0;
          uint64_t want_mag  = cases[k] < 0 ? (~(uint64_t)cases[k] + 1u) : (uint64_t)cases[k];
          CHECK(rec[0] == 1 && rec[1] == want_sign && rec[3] == want_mag,
                "records reads carrier value exactly (INT64_MIN => sign=1, mag=2^63, never 0)");
        }
        free(rec);
        slate_array_free(a);
      }
      slate_dag_free(b);
    }
  }

  /* 10. Regression (fragment param slot out of range): a crc-valid fragment whose root reads a param slot
   *     larger than the run's dim count must refuse at dispatch — never an out-of-bounds Load (a SIGSEGV, or a
   *     leaked heap word returned as the value). The run buffer is [ params | carrier ] with exactly one param
   *     slot here (dims = {1}); a param leaf at slot (1<<20) would read ~8 MB past the buffer. Build it through
   *     the real API so the fragment is genuinely crc-valid, then splice + run with fewer dims than it uses. */
  {
    SlateDag *b = slate_dag_new();
    MS_INSTALL(b, &store);
    int32_t p = slate_dag_param(b, 1 << 20);                    /* slot far beyond any run's dims */
    int32_t root = slate_dag_add(b, p, slate_dag_lit(b, 0));    /* root reads param(1<<20) */
    Prog fr = {0};
    uint32_t none = 0;
    CHECK(slate_dag_save_fragment(b, root, &none, 0, &fr.w, &fr.wn) == NULL, "oob-param fragment kept");
    slate_dag_free(b);

    SlateFrag *f = load_by(fr.w, fr.wn, NULL);
    CHECK(f != NULL, "oob-param fragment loads (crc-valid)");
    if (f) {
      SlateDag *b2 = slate_dag_new();
      MS_INSTALL(b2, &store);
      int32_t r = -1; slate_dag_splice(b2, f, NULL, 0, &r);
      CHECK(r >= 0, "oob-param fragment splices");
      if (r >= 0) {
        int64_t dims[1] = {1};                                  /* only 1 param provided — slot 1<<20 is OOB */
        SlateArray *a = slate_dag_run(b2, r, dims, 1);
        CHECK(a == NULL, "out-of-range param slot refuses at dispatch (no SIGSEGV / no leaked word)");
        if (a) slate_array_free(a);
      }
      slate_dag_free(b2);
      slate_frag_free(f);
    }
    prog_free(&fr);
  }

  prog_free(&frag);
  ms_free(&store);
  if (fails == 0) printf("PASS frag_cabi: all fragment/prim checks\n");
  return fails ? 1 : 0;
}
