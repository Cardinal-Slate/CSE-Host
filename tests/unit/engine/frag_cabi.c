/* The fragment-library door from pure C (slate/slate.h). No C++ in this translation unit. Builds a fragment
 * with one hole (A) and one baked constant carrier (C), computing root[i] = A[i]*2 + C[i] over grid[i];
 * serializes it to memory, loads it back, checks the interface, splices it into two fresh builders with
 * different A data, runs, and verifies the exact cells. Then it exercises robustness (bad magic / bad crc /
 * truncation all refuse), the size-then-fill iface path, splice typecheck refusals, and the mulh/asr prims. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"
#include "slate/stream.h"

/* ---- a growable memory sink and a cursor source over the same bytes ---- */
typedef struct { unsigned char *buf; size_t len, cap; } MemSink;
static uint64_t mem_sink(const void *bytes, uint64_t n, void *user) {
  MemSink *m = (MemSink *)user;
  if (m->len + n > m->cap) { m->cap = (m->len + n) * 2 + 64; m->buf = (unsigned char *)realloc(m->buf, m->cap); }
  memcpy(m->buf + m->len, bytes, (size_t)n); m->len += (size_t)n; return n;
}
typedef struct { const unsigned char *buf; size_t len, pos; } MemSrc;
static uint64_t mem_src(void *bytes, uint64_t n, void *user) {
  MemSrc *s = (MemSrc *)user;
  if (s->pos + n > s->len) return 0;                 /* short read → the loader refuses */
  memcpy(bytes, s->buf + s->pos, (size_t)n); s->pos += (size_t)n; return n;
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL frag_cabi: %s\n", msg); fails++; } } while (0)

/* build root[i] = A[i]*2 + C[i], A a hole, C baked = {10,20,30,40}; return the serialized fragment bytes. */
static MemSink build_and_save(void) {
  SlateDag *b = slate_dag_new();
  int64_t placeholderA[4] = {0, 0, 0, 0};            /* the hole needs data to build+typecheck once */
  int64_t bakedC[4] = {10, 20, 30, 40};
  uint32_t cidA = slate_dag_carrier(b, placeholderA, 4);
  uint32_t cidC = slate_dag_carrier(b, bakedC, 4);
  int32_t p = slate_dag_param(b, 0);
  int32_t lA = slate_dag_load(b, cidA, p);
  int32_t lC = slate_dag_load(b, cidC, p);
  int32_t m = slate_dag_mul(b, lA, slate_dag_lit(b, 2));
  int32_t root = slate_dag_add(b, m, lC);
  MemSink out = {0};
  uint32_t holes[1] = {cidA};
  const char *rc = slate_dag_save_fragment(b, root, holes, 1, mem_sink, &out);
  CHECK(rc == NULL, "save_fragment ok");
  slate_dag_free(b);
  return out;
}

/* splice the fragment over A, run on grid[4], and read the 4 int64 cells into out[4]. */
static int splice_run(const MemSink *frag, const int64_t A[4], int64_t out[4]) {
  MemSrc src = {frag->buf, frag->len, 0};
  SlateFrag *f = slate_frag_load(mem_src, &src);
  if (!f) return -1000;
  SlateDag *b = slate_dag_new();
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

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);

  /* 1. round-trip + splice + run */
  MemSink frag = build_and_save();
  CHECK(frag.len > 0, "fragment serialized nonempty");

  /* 2. iface */
  {
    MemSrc src = {frag.buf, frag.len, 0};
    SlateFrag *f = slate_frag_load(mem_src, &src);
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

  /* 4. robustness: corrupt magic, corrupt crc, truncate — every one refuses (NULL), no crash */
  {
    unsigned char *bad = (unsigned char *)malloc(frag.len);
    /* bad magic */
    memcpy(bad, frag.buf, frag.len); bad[0] ^= 0xFF;
    { MemSrc s = {bad, frag.len, 0}; CHECK(slate_frag_load(mem_src, &s) == NULL, "bad magic refused"); }
    /* corrupt a body byte (crc mismatch) — flip a byte in the middle */
    memcpy(bad, frag.buf, frag.len); bad[frag.len / 2] ^= 0x01;
    { MemSrc s = {bad, frag.len, 0}; CHECK(slate_frag_load(mem_src, &s) == NULL, "crc mismatch refused"); }
    /* truncated stream */
    { MemSrc s = {frag.buf, frag.len - 3, 0}; CHECK(slate_frag_load(mem_src, &s) == NULL, "truncation refused"); }
    free(bad);
  }

  /* 4b. count-amplification hardening: a huge count field in the header with a short stream must refuse (the
   * incremental reader runs the stream dry) instead of resizing to billions of elements ahead of the crc
   * check (OOM-DoS). Header layout is 8 u64: [magic, version, flags, ninstr, root, nparams, ncarrier,
   * potential] → ninstr at byte 24, ncarrier at byte 48. */
  {
    uint64_t huge = 0xFFFFFFFFULL;                 /* ~4 billion elements: many GB if resized up front */
    unsigned char *bad = (unsigned char *)malloc(frag.len);
    memcpy(bad, frag.buf, frag.len); memcpy(bad + 24, &huge, 8);   /* ninstr := huge */
    { MemSrc s = {bad, frag.len, 0}; CHECK(slate_frag_load(mem_src, &s) == NULL, "huge ninstr refused (no OOM)"); }
    memcpy(bad, frag.buf, frag.len); memcpy(bad + 48, &huge, 8);   /* ncarrier := huge */
    { MemSrc s = {bad, frag.len, 0}; CHECK(slate_frag_load(mem_src, &s) == NULL, "huge ncarrier refused (no OOM)"); }
    free(bad);
  }

  /* 5. splice typecheck / arg errors: wrong arity and an out-of-range carrier id both refuse cleanly */
  {
    MemSrc src = {frag.buf, frag.len, 0};
    SlateFrag *f = slate_frag_load(mem_src, &src);
    SlateDag *b = slate_dag_new();
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

  /* 7. a fragment that uses asr round-trips (flags bit set, still loads/splices/runs) */
  {
    SlateDag *b = slate_dag_new();
    int64_t placeholder[4] = {0, 0, 0, 0};
    uint32_t cid = slate_dag_carrier(b, placeholder, 4);
    int32_t p = slate_dag_param(b, 0);
    int32_t l = slate_dag_load(b, cid, p);
    int32_t root = slate_dag_asr(b, l, slate_dag_lit(b, 1));    /* root[i] = A[i] >> 1 */
    MemSink out = {0};
    uint32_t holes[1] = {cid};
    CHECK(slate_dag_save_fragment(b, root, holes, 1, mem_sink, &out) == NULL, "asr fragment saved");
    slate_dag_free(b);

    int64_t A[4] = {8, 9, 10, 100}, res[4] = {0};
    int rc = splice_run(&out, A, res);
    CHECK(rc == 0, "asr fragment splice_run ok");
    CHECK(res[0] == 4 && res[1] == 4 && res[2] == 5 && res[3] == 50, "asr fragment values");
    free(out.buf);
  }

  /* 8. token-based repeated reload — the durable-value-across-a-checkpoint capability that used to be
   *    asserted on a bare node id (surface.cpp block 17b, now single-load resume): save the fragment
   *    once, then frag_load + splice + run the same serialized token repeatedly (3 cycles) with the same
   *    input, and check the value reproduces identically each cycle. Position-independent: every cycle
   *    loads into a fresh builder. This is the receipt-oriented durable handle that the checkpoint
   *    bare-id path was retired in favor of. */
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
    int32_t p = slate_dag_param(b, 1 << 20);                    /* slot far beyond any run's dims */
    int32_t root = slate_dag_add(b, p, slate_dag_lit(b, 0));    /* root reads param(1<<20) */
    MemSink fr = {0};
    uint32_t none = 0;
    CHECK(slate_dag_save_fragment(b, root, &none, 0, mem_sink, &fr) == NULL, "oob-param fragment saved");
    slate_dag_free(b);

    MemSrc s = {fr.buf, fr.len, 0};
    SlateFrag *f = slate_frag_load(mem_src, &s);
    CHECK(f != NULL, "oob-param fragment loads (crc-valid)");
    if (f) {
      SlateDag *b2 = slate_dag_new();
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
    free(fr.buf);
  }

  free(frag.buf);
  if (fails == 0) printf("PASS frag_cabi: all fragment/prim checks\n");
  return fails ? 1 : 0;
}
