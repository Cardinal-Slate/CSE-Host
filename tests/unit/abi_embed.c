/* tests/unit/abi_embed.c — the C ABI, embedder band (slate/embed.h), from pure C. No C++ in this translation unit.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 *
 * Gate: an effect fires only under a grant and an installed host, else it refuses to a bottom without reaching the
 * host; an effect's result is a row — the same request in another builder over the same store is served without
 * the host; the deadline gate aborts an effect to a clean refusal; async hosts park fibers, not threads — tasks
 * spawned in a scope overlap their waits and each reads its own value; the search over a graph measures by shape
 * and aggregates by computation.
 *
 * Every claim below holds whether or not the store already answers. The store is the only memory, so a gate
 * that needed an empty one would state a property of a cold process rather than of the engine: a granted ask
 * whose row is there is served from it and the host is never called, which is the design working. The
 * fail-closed claims are therefore put the way they are actually true — the host is never reached without a
 * grant, a host, and a gate that permits it — rather than as "the effect refuses", which holds only on a miss. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "slate/slate.h"
#include "slate/embed.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- a host: answers every effect with one 8-byte LE value, counting how often it is reached ---- */
typedef struct { int calls; int64_t reply; } Host;
static int host_perform(const char *cls, const uint8_t *req, uint64_t reqn, uint8_t **out, uint64_t *outn, void *u) {
  (void)cls; (void)req; (void)reqn; Host *h = (Host *)u; h->calls++;
  uint8_t *b = (uint8_t *)malloc(8); uint64_t v = (uint64_t)h->reply; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
  *out = b; *outn = 8; return 0;
}
/* ---- the store: rows behind decode/put ---- */
typedef struct { uint8_t *w; uint64_t wn; uint8_t *b; uint64_t n; } Row;
typedef struct { Row *r; size_t n, cap; long hits, puts; } Store;
static int st_get(const uint8_t *w, uint64_t wn, const uint8_t *s, uint64_t sn, uint8_t **out, uint64_t *outn, void *u) {
  (void)s; (void)sn; Store *st = (Store *)u;
  for (size_t i = 0; i < st->n; i++) if (st->r[i].wn == wn && memcmp(st->r[i].w, w, (size_t)wn) == 0) {
    st->hits++; *out = (uint8_t *)malloc(st->r[i].n ? (size_t)st->r[i].n : 1); memcpy(*out, st->r[i].b, (size_t)st->r[i].n); *outn = st->r[i].n; return 0; }
  return 1;
}
static int st_put(const uint8_t *w, uint64_t wn, const uint8_t *b, uint64_t n, const uint8_t *s, uint64_t sn, void *u) {
  (void)s; (void)sn; Store *st = (Store *)u; st->puts++;
  if (st->n == st->cap) { st->cap = st->cap ? st->cap * 2 : 64; st->r = (Row *)realloc(st->r, st->cap * sizeof(Row)); }
  Row *r = &st->r[st->n++]; r->w = (uint8_t *)malloc((size_t)wn); memcpy(r->w, w, (size_t)wn); r->wn = wn;
  r->b = (uint8_t *)malloc(n ? (size_t)n : 1); memcpy(r->b, b, (size_t)n); r->n = n; return 0;
}
static void st_free(Store *st) { for (size_t i = 0; i < st->n; i++) { free(st->r[i].w); free(st->r[i].b); } free(st->r); memset(st, 0, sizeof *st); }

/* run `root` over one cell; 0 with *v on an exact int64 value, -1 on a refused (bottom) reading */
static int run1(SlateDag *b, int32_t root, int64_t *v) {
  int64_t one = 1; SlateArray *a = slate_dag_run(b, root, &one, 1);
  if (!a) return -1;
  const slate_entry *e; int32_t n; slate_array_receipt(a, &e, &n);
  int ok = !slate_entry_is(e, n, "verdict", "undefined");   /* an effect's value is empirical: verdict bracket, never exact */
  int64_t num = 0, den = 1; if (ok) ok = slate_array_i64_unsafe(a, &num, &den) == NULL && den == 1;
  slate_array_free(a); if (ok) *v = num; return ok ? 0 : -1;
}
static const char *const CAP_GENERIC[] = { "generic" };
static const char *const CAP_OTHER[] = { "other" };
static int32_t fx(SlateDag *b, const char *blob) { return slate_dag_effect(b, "generic", (const uint8_t *)blob, strlen(blob), NULL, 0); }

/* ---- async: begin hands the slot to a thread that completes it after a short wait ---- */
typedef struct { void *slot; int64_t reply; int ms; } Pending;
static void *completer(void *u) {
  Pending *p = (Pending *)u; usleep((useconds_t)p->ms * 1000);
  uint8_t d[8]; uint64_t v = (uint64_t)p->reply; for (int i = 0; i < 8; i++) d[i] = (uint8_t)(v >> (8 * i));
  slate_effect_complete(p->slot, d, 8); free(p); return NULL;
}
typedef struct { int begins; int64_t reply; int ms; } AsyncHost;
static int host_begin(const char *cls, const uint8_t *req, uint64_t reqn, void *slot, void *u) {
  (void)cls; (void)req; (void)reqn; AsyncHost *h = (AsyncHost *)u; h->begins++;
  Pending *p = (Pending *)malloc(sizeof *p); p->slot = slot; p->reply = h->reply; p->ms = h->ms;
  pthread_t t; pthread_create(&t, NULL, completer, p); pthread_detach(t); return 0;
}
typedef struct { AsyncHost host; Store st; int64_t got; int rc; const char *blob; } Task;
static void task_run(void *u) {
  Task *t = (Task *)u; SlateDag *b = slate_dag_new();
  /* this block's subject is the parking, not the value, and a fiber only parks on a miss: a granted ask whose
     row is there is served without the host ever beginning. So the task brings memory of its own — the one
     place in this gate where an empty store is the thing under test rather than a convenience. */
  slate_dag_codec(b, NULL, st_get, st_put, NULL, 0, &t->st);
  slate_dag_effect_caps(b, CAP_GENERIC, 1); slate_dag_effect_host(b, NULL, host_begin, &t->host);
  t->rc = run1(b, slate_dag_add(b, fx(b, t->blob), slate_dag_lit(b, 1)), &t->got);
  slate_dag_free(b); st_free(&t->st);
}
static void scope_body(SlateScope *s, void *u) { Task *ts = (Task *)u; for (int i = 0; i < 4; i++) slate_scope_spawn(s, task_run, &ts[i]); }

static int proceed_no(void *u) { (*(int *)u)++; return 0; }

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);

  /* ---- 1. grant + host: an effect resolves to the host's value; over a store the same request is one host call ---- */
  {
    Host h = { 0, 41 }; Store st; memset(&st, 0, sizeof st); SlateDag *b = slate_dag_new(); slate_dag_codec(b, NULL, st_get, st_put, NULL, 0, &st);
    slate_dag_effect_caps(b, CAP_GENERIC, 1); slate_dag_effect_host(b, host_perform, NULL, &h);
    int64_t v = 0; CHECK(run1(b, slate_dag_add(b, fx(b, "req-1"), slate_dag_lit(b, 1)), &v) == 0 && v == 42, "1: effect+1 read %lld", (long long)v);
    CHECK(h.calls <= 1, "1: the host was reached %d times for one request (want at most 1)", h.calls);
    int first = h.calls;
    CHECK(run1(b, slate_dag_add(b, fx(b, "req-1"), fx(b, "req-1")), &v) == 0 && v == 82 && h.calls == first,
          "1: the same request reached the host again (%d)", h.calls);
    CHECK(run1(b, fx(b, "req-2"), &v) == 0 && v == 41 && h.calls <= first + 1,
          "1: a distinct request cost more than one call (%d)", h.calls);
    slate_dag_free(b); st_free(&st);
    printf("  grant+host: the effect reads the host's value (empirical); one request costs at most one call, and is its row after\n");
  }

  /* ---- 2. the row outlives the builder: over a shared store a second builder is served without the host ---- */
  {
    Store st; memset(&st, 0, sizeof st); Host h = { 0, 7 };
    SlateDag *b1 = slate_dag_new(); slate_dag_codec(b1, NULL, st_get, st_put, NULL, 0, &st);
    slate_dag_effect_caps(b1, CAP_GENERIC, 1); slate_dag_effect_host(b1, host_perform, NULL, &h);
    int64_t v = 0; CHECK(run1(b1, fx(b1, "row"), &v) == 0 && v == 7 && h.calls <= 1, "2: first builder");
    SlateDag *b2 = slate_dag_new(); slate_dag_codec(b2, NULL, st_get, st_put, NULL, 0, &st);
    slate_dag_effect_caps(b2, CAP_GENERIC, 1); slate_dag_effect_host(b2, host_perform, NULL, &h);
    int after1 = h.calls;
    long hits = st.hits; CHECK(run1(b2, fx(b2, "row"), &v) == 0 && v == 7 && h.calls == after1 && st.hits > hits,
          "2: the second builder reached the host (%d) / hit %ld", h.calls, st.hits - hits);
    /* the grant is checked before the store: without it the row is not served either */
    SlateDag *b3 = slate_dag_new(); slate_dag_codec(b3, NULL, st_get, st_put, NULL, 0, &st); slate_dag_effect_host(b3, host_perform, NULL, &h);
    CHECK(run1(b3, fx(b3, "row"), &v) != 0 && h.calls == after1, "2: an ungranted builder was served the row");
    slate_dag_free(b1); slate_dag_free(b2); slate_dag_free(b3); st_free(&st);
    printf("  row: the effect's result is a row — a second builder over the store is served, the host untouched; no grant, no row\n");
  }

  /* ---- 3. fail-closed: no grant, the wrong grant, no host, the gate says no — the host is never reached ---- */
  {
    Host h = { 0, 5 }; int64_t v = 0; int asked = 0;
    SlateDag *b = slate_dag_new(); slate_dag_effect_host(b, host_perform, NULL, &h);
    /* the grant is checked before the store is consulted, so these two refuse whether or not a row exists. */
    CHECK(run1(b, fx(b, "x"), &v) != 0, "3: no grant did not refuse");
    slate_dag_effect_caps(b, CAP_OTHER, 1); CHECK(run1(b, fx(b, "x"), &v) != 0, "3: the wrong grant did not refuse");
    /* these two are answered from a row where there is one, so the claim is the one that always holds: the
       host is not reached. Withholding it, or refusing at the gate, never causes a call. */
    slate_dag_effect_caps(b, CAP_GENERIC, 1); slate_dag_effect_host(b, NULL, NULL, NULL); run1(b, fx(b, "x"), &v);
    slate_dag_effect_host(b, host_perform, NULL, &h); slate_dag_proceed(b, proceed_no, &asked);
    run1(b, fx(b, "x"), &v);
    CHECK(h.calls == 0, "3: the host was reached %d times with no grant, no host, or a gate that said no", h.calls);
    slate_dag_proceed(b, NULL, NULL);
    CHECK(run1(b, fx(b, "x"), &v) == 0 && v == 5, "3: granted, hosted and permitted, the effect has no value");
    CHECK(h.calls <= 1, "3: the host was reached %d times for one request", h.calls);
    slate_dag_free(b);
    printf("  fail-closed: no grant refuses; withholding the host or refusing at the gate never reaches it; permitted, the value is there\n");
  }

  /* ---- 4. fibers: four tasks with async hosts overlap their waits on one scope; each reads its own value ---- */
  {
    Task ts[4]; const char *blobs[4] = { "a", "b", "c", "d" };
    for (int i = 0; i < 4; i++) { memset(&ts[i], 0, sizeof ts[i]); ts[i].host.reply = 100 + i; ts[i].host.ms = 30; ts[i].rc = -1; ts[i].blob = blobs[i]; }
    slate_scope_run(scope_body, ts);
    for (int i = 0; i < 4; i++)
      CHECK(ts[i].rc == 0 && ts[i].got == 101 + i && ts[i].host.begins == 1,
            "4: task %d rc %d got %lld begins %d", i, ts[i].rc, (long long)ts[i].got, ts[i].host.begins);
    printf("  fibers: four tasks in one scope, each parked on its own async host and woke with its own value\n");
  }

  /* ---- 5. search: by shape (measurement) and by computation (the aggregate of a collection) ---- */
  {
    SlateDag *b = slate_dag_new();
    int32_t leaves[8]; for (int i = 0; i < 8; i++) leaves[i] = slate_dag_lit(b, 10 + i);
    int32_t s01 = slate_dag_add(b, leaves[0], leaves[1]), s23 = slate_dag_add(b, leaves[2], leaves[3]);
    int32_t s45 = slate_dag_add(b, leaves[4], leaves[5]), s67 = slate_dag_add(b, leaves[6], leaves[7]);
    int32_t root = slate_dag_add(b, slate_dag_add(b, s01, s23), slate_dag_add(b, s45, s67));
    int32_t out[32]; uint32_t cnt = 0; uint64_t visited = 0; int32_t resumable = 0;
    /* measurement: the compose-shaped nodes at the leaves' height — the walk reports how many it found and visited */
    const char *rc = slate_dag_search(b, root, 0, "compose", 0, out, 32, &cnt, &visited, &resumable, -1, NULL, -1, NULL, NULL, NULL);
    CHECK(rc == NULL && visited >= 15 && resumable == 0, "5: measurement rc=%s visited=%llu", rc ? rc : "ok", (unsigned long long)visited);
    /* a budget cuts the walk and says so */
    rc = slate_dag_search(b, root, 0, "compose", 3, out, 32, &cnt, &visited, &resumable, -1, NULL, -1, NULL, NULL, NULL);
    CHECK(rc == NULL && resumable == 1, "5: a budget of 3 did not cut the walk");
    /* computation: the add-built collection aggregates to the exact fold of its leaves */
    int64_t num = 0, den = 0; int32_t undef = 0; int32_t zero = slate_dag_lit(b, 0);
    rc = slate_dag_search(b, root, 0, NULL, 0, out, 32, &cnt, &visited, &resumable, zero, NULL, root, &num, &den, &undef);
    CHECK(rc == NULL && !undef && num == 108 && den == 1, "5: aggregate rc=%s %lld/%lld undef=%d", rc ? rc : "ok", (long long)num, (long long)den, undef);
    CHECK(slate_refused(slate_dag_search(b, root, 0, "sideways", 0, NULL, 0, NULL, NULL, NULL, -1, NULL, -1, NULL, NULL, NULL), "args"), "5: a mode that is not a name was not refused");
    slate_dag_free(b);
    printf("  search: by shape walks %llu vertices (a budget cuts it, resumable); by computation the collection folds to 108\n", (unsigned long long)visited);
  }

  /* ---- 6. the lens: a share of the prime set is a different row, and a share never half-answers ---- */
  {
    Store st; memset(&st, 0, sizeof st);
    const int64_t a3[3] = { 1000003, 1000033, 1000037 };
    const int64_t b3[3] = { 1000039, 1000081, 1000099 };
    long put_unpinned = 0, put_a = 0, put_b = 0, put_a_again = 0;

    /* the same construction, three times: unpinned, then two disjoint shares, over one store */
    for (int pass = 0; pass < 4; pass++) {
      SlateDag *b = slate_dag_new();
      CHECK(b != NULL, "6: builder");
      slate_dag_codec(b, NULL, st_get, st_put, NULL, 0, &st);
      if (pass == 1 || pass == 3) CHECK(slate_dag_lens(b, a3, 3) == NULL, "6: pin a refused");
      if (pass == 2)              CHECK(slate_dag_lens(b, b3, 3) == NULL, "6: pin b refused");
      uint32_t c = slate_dag_carrier(b, NULL, 0);
      const int64_t vals[4] = { 700001, -900002, 1500003, -20004 };
      slate_dag_carrier_set(b, c, vals, 4);
      int32_t i0 = slate_dag_param(b, 0);
      int32_t root = slate_dag_add(b, slate_dag_load(b, c, i0), slate_dag_lit(b, 7));
      const int64_t dims[1] = { 4 };
      long before = st.puts;
      SlateArray *arr = slate_dag_run(b, root, dims, 1);
      long wrote = st.puts - before;
      if (pass == 0) put_unpinned = wrote;
      else if (pass == 1) put_a = wrote;
      else if (pass == 2) put_b = wrote;
      else put_a_again = wrote;
      CHECK(arr != NULL, "6: pass %d produced no reading", pass);
      slate_array_free(arr);
      slate_dag_free(b);
    }
    /* each of the three named a row of its own: unpinned, share a, share b */
    CHECK(put_unpinned > 0, "6: the unpinned run kept nothing");
    CHECK(put_a > 0, "6: share a kept nothing — it was served the unpinned row");
    CHECK(put_b > 0, "6: share b kept nothing — it was served another share's row");
    /* and asking for share a a second time is a hit: the same share names the same row */
    CHECK(put_a_again == 0, "6: share a wrote %ld rows the second time — its name is not stable", put_a_again);
    printf("  lens: one construction, three prime sets, three rows; the same share twice is one row\n");

    /* the door refuses what would make a bad modulus, rather than accepting it */
    SlateDag *g = slate_dag_new();
    const int64_t too_big[1] = { (int64_t)1 << 24 };
    const int64_t dup[2] = { 1000003, 1000003 };
    const int64_t one[1] = { 1000003 };
    const int64_t nonprime[1] = { 1000001 };   /* 101 * 9901: composite, in range, not a repeat */
    CHECK(slate_dag_lens(g, too_big, 1) != NULL, "6: a prime past the lane's width was accepted");
    CHECK(slate_dag_lens(g, dup, 2) != NULL, "6: a repeated prime was accepted");
    CHECK(slate_dag_lens(g, NULL, 2) != NULL, "6: a null share with k>0 was accepted");
    CHECK(slate_dag_lens(NULL, one, 1) != NULL, "6: a null builder was accepted");
    CHECK(slate_refused(slate_dag_lens(g, nonprime, 1), "args"), "6: a non-prime was accepted");
    CHECK(slate_dag_lens(g, NULL, 0) == NULL, "6: unpinning was refused");
    slate_dag_free(g);
    printf("  lens: a prime past the lane's width, a repeat, a null share, and a non-prime are each refused\n");
  }

  /* ---- 7. a program is a leaf: emit names it, run and tail take the name, a name the store lacks refuses.
     A name is `word ‖ count` — the word, then the leaf's byte count as 8 little-endian bytes — so it is 8
     bytes longer than a word. ---- */
  {
    Store st; memset(&st, 0, sizeof st);
    static const char *const CAP_SELF[] = { "slate.emit", "slate.run", "slate.tail" };
    int64_t v = 0;

    /* the width of a word over this codec, observed from the first row any dispatch keeps */
    SlateDag *b0 = slate_dag_new(); slate_dag_codec(b0, NULL, st_get, st_put, NULL, 0, &st);
    run1(b0, slate_dag_add(b0, slate_dag_lit(b0, 1), slate_dag_lit(b0, 1)), &v); slate_dag_free(b0);
    CHECK(st.n > 0, "7: no row to learn the word width from");
    const uint64_t wn = st.n ? st.r[0].wn : 0, nn = wn + 8;   /* a word, and a name: the word and its count */

    /* emit: the sub-DAG 20+22 becomes a leaf; the carrier holds its name, nothing else */
    SlateDag *b = slate_dag_new(); slate_dag_codec(b, NULL, st_get, st_put, NULL, 0, &st);
    slate_dag_effect_caps(b, CAP_SELF, 3);
    int32_t sub = slate_dag_add(b, slate_dag_lit(b, 20), slate_dag_lit(b, 22));
    uint32_t cw = slate_dag_effect_array(b, "slate.emit", NULL, 0, &sub, 1, (int64_t)nn);
    CHECK(cw != UINT32_MAX, "7: emit refused at build");
    /* run by name: the verdict carrier holds the program's reading as 8 LE byte-cells */
    uint32_t cv = slate_dag_effect_io(b, "slate.run", NULL, 0, &cw, 1, 8);
    CHECK(cv != UINT32_MAX, "7: run refused at build");
    long rows_before = (long)st.n;
    CHECK(run1(b, slate_dag_load(b, cv, slate_dag_lit(b, 0)), &v) == 0 && v == 42, "7: run by name read %lld (want 42)", (long long)v);
    CHECK((long)st.n > rows_before, "7: emit kept no row");
    /* the name itself, cell by cell, so another container can run the program with nothing but its name */
    uint8_t *word = (uint8_t *)calloc((size_t)nn, 1);
    int word_ok = 1;
    for (uint64_t i = 0; i < nn; i++) {
      if (run1(b, slate_dag_load(b, cw, slate_dag_lit(b, (int64_t)i)), &v) != 0) { word_ok = 0; break; }
      word[i] = (uint8_t)v;
    }
    CHECK(word_ok, "7: the emitted name could not be read back");
    { uint64_t pn = 0; for (int i = 0; i < 8; i++) pn |= (uint64_t)word[wn + i] << (8 * i);
      CHECK(pn > 0, "7: the name carries no byte count"); }
    slate_dag_free(b);

    /* a second container over the same store: the name alone runs the program — no bytes crossed */
    SlateDag *b2 = slate_dag_new(); slate_dag_codec(b2, NULL, st_get, st_put, NULL, 0, &st);
    slate_dag_effect_caps(b2, CAP_SELF, 3);
    uint32_t c2 = slate_dag_carrier_bytes(b2, word, nn);
    uint32_t cv2 = slate_dag_effect_io(b2, "slate.run", NULL, 0, &c2, 1, 8);
    CHECK(run1(b2, slate_dag_load(b2, cv2, slate_dag_lit(b2, 0)), &v) == 0 && v == 42,
          "7: a second container could not run the program by name (%lld)", (long long)v);
    /* the tail: hand off by name; the trampoline resolves it and the dispatch's reading is the program's */
    uint32_t ct = slate_dag_effect_io(b2, "slate.tail", NULL, 0, &c2, 1, 8);
    CHECK(run1(b2, slate_dag_load(b2, ct, slate_dag_lit(b2, 0)), &v) == 0 && v == 42,
          "7: the tail hand-off by name did not run the program (%lld)", (long long)v);
    slate_dag_free(b2);

    /* a name the store does not hold refuses: a program that was never kept does not exist */
    SlateDag *b3 = slate_dag_new(); slate_dag_codec(b3, NULL, st_get, st_put, NULL, 0, &st);
    slate_dag_effect_caps(b3, CAP_SELF, 3);
    uint8_t *junk = (uint8_t *)malloc((size_t)nn);       /* the same count, a word nothing was kept under */
    memcpy(junk, word, (size_t)nn);
    for (uint64_t i = 0; i < wn; i++) junk[i] = (uint8_t)(word[i] ^ 0xA5);
    uint32_t c3 = slate_dag_carrier_bytes(b3, junk, nn);
    uint32_t cv3 = slate_dag_effect_io(b3, "slate.run", NULL, 0, &c3, 1, 8);
    CHECK(run1(b3, slate_dag_load(b3, cv3, slate_dag_lit(b3, 0)), &v) != 0, "7: an unkept name ran (%lld)", (long long)v);
    uint32_t ct3 = slate_dag_effect_io(b3, "slate.tail", NULL, 0, &c3, 1, 8);
    CHECK(run1(b3, slate_dag_load(b3, ct3, slate_dag_lit(b3, 0)), &v) != 0, "7: a tail to an unkept name ran (%lld)", (long long)v);
    slate_dag_free(b3);
    free(word); free(junk); st_free(&st);
    printf("  program: emit keeps a leaf and yields its name; run and tail take the name, in this container or another; an unkept name refuses\n");
  }

  /* ---- 8. the program is context: set the name on the container, start it; no graph is built ---- */
  {
    Store st; memset(&st, 0, sizeof st);
    static const char *const CAP_SELF[] = { "slate.emit", "slate.run", "slate.tail" };
    int64_t v = 0;
    SlateDag *b0 = slate_dag_new(); slate_dag_codec(b0, NULL, st_get, st_put, NULL, 0, &st);
    run1(b0, slate_dag_add(b0, slate_dag_lit(b0, 1), slate_dag_lit(b0, 1)), &v); slate_dag_free(b0);
    const uint64_t wn = st.n ? st.r[0].wn : 0, nn = wn + 8;

    /* one container emits the leaf 20+22 and reads back its name */
    SlateDag *b = slate_dag_new(); slate_dag_codec(b, NULL, st_get, st_put, NULL, 0, &st);
    slate_dag_effect_caps(b, CAP_SELF, 3);
    int32_t sub = slate_dag_add(b, slate_dag_lit(b, 20), slate_dag_lit(b, 22));
    uint32_t cw = slate_dag_effect_array(b, "slate.emit", NULL, 0, &sub, 1, (int64_t)nn);
    uint8_t *word = (uint8_t *)calloc((size_t)nn, 1);
    int word_ok = 1;
    for (uint64_t i = 0; i < nn; i++) {
      if (run1(b, slate_dag_load(b, cw, slate_dag_lit(b, (int64_t)i)), &v) != 0) { word_ok = 0; break; }
      word[i] = (uint8_t)v;
    }
    CHECK(word_ok, "8: the emitted name could not be read back");
    slate_dag_free(b);

    /* the entrypoint: a store, a word, start — the reading is the program's */
    const int64_t one = 1;
    SlateDag *e = slate_dag_new(); slate_dag_codec(e, NULL, st_get, st_put, NULL, 0, &st);
    CHECK(slate_dag_start(e, &one, 1) == NULL, "8: a container with no program started");
    CHECK(slate_dag_program(e, word, nn) == NULL, "8: setting the program refused");
    SlateArray *a = slate_dag_start(e, &one, 1);
    CHECK(a != NULL, "8: start refused a kept program");
    if (a) {
      int64_t num = 0, den = 1;
      CHECK(slate_array_i64_unsafe(a, &num, &den) == NULL && den == 1 && num == 42, "8: start read %lld (want 42)", (long long)num);
      slate_array_free(a);
    }
    /* clearing it, and a word the store does not hold, each refuse to start */
    CHECK(slate_dag_program(e, NULL, 0) == NULL, "8: clearing the program refused");
    CHECK(slate_dag_start(e, &one, 1) == NULL, "8: a cleared program started");
    uint8_t *junk = (uint8_t *)malloc((size_t)nn);
    memcpy(junk, word, (size_t)nn);
    for (uint64_t i = 0; i < wn; i++) junk[i] = (uint8_t)(word[i] ^ 0x5A);
    slate_dag_program(e, junk, nn);
    CHECK(slate_dag_start(e, &one, 1) == NULL, "8: an unkept program started");
    /* a word with no count is not a name: it names nothing to start */
    CHECK(slate_dag_program(e, word, wn) == NULL, "8: setting a bare word refused");
    CHECK(slate_dag_start(e, &one, 1) == NULL, "8: a bare word started");
    CHECK(slate_dag_program(e, word, nn) == NULL, "8: setting the program refused");
    CHECK(slate_dag_program(NULL, word, nn) != NULL, "8: a null builder was accepted");
    CHECK(slate_dag_program(e, NULL, 4) != NULL, "8: a null name with n > 0 was accepted");
    slate_dag_free(e);
    free(word); free(junk); st_free(&st);
    printf("  program: the leaf a container runs is context; start runs it, and refuses with none, a cleared one, a bare word, or an unkept one\n");
  }

  /* ---- 9. the roster, the share, the ask and the cell word: the doors a placement seam uses ---- */
  {
    Store st; memset(&st, 0, sizeof st);
    /* a roster of six primes divided into three units: shares are [0,2), [2,4), [4,6) */
    const int64_t roster[6] = { 1000003, 1000033, 1000037, 1000039, 1000081, 1000099 };
    const int64_t share0[2] = { 1000003, 1000033 };
    const int64_t share1[2] = { 1000037, 1000039 };
    const int64_t astride[2] = { 1000033, 1000037 };   /* two real primes of the roster, but not one share */
    const int64_t foreign[2] = { 1000117, 1000121 };   /* not in the roster at all */

    SlateDag *b = slate_dag_new();
    slate_dag_codec(b, NULL, st_get, st_put, NULL, 0, &st);
    CHECK(slate_dag_roster(b, roster, 6, 3) == NULL, "9: the roster was refused");
    /* the roster takes the same prime rule as the lens */
    { const int64_t nonprime[1] = { 1000001 };
      CHECK(slate_refused(slate_dag_roster(b, nonprime, 1, 1), "args"), "9: a non-prime roster was accepted");
      const int64_t dup2[2] = { 1000003, 1000003 };
      CHECK(slate_refused(slate_dag_roster(b, dup2, 2, 1), "args"), "9: a repeated roster prime was accepted");
      CHECK(slate_dag_roster(NULL, roster, 6, 3) != NULL, "9: a null builder was accepted");
      CHECK(slate_dag_roster(b, NULL, 3, 3) != NULL, "9: a null roster with k>0 was accepted");
      CHECK(slate_dag_roster(b, roster, 6, 3) == NULL, "9: the roster was refused on the way back"); }
    /* the lens must be exactly one share of it */
    CHECK(slate_refused(slate_dag_lens(b, astride, 2), "args"), "9: a lens astride the split was accepted");
    CHECK(slate_refused(slate_dag_lens(b, foreign, 2), "args"), "9: a lens outside the roster was accepted");
    CHECK(slate_refused(slate_dag_lens(b, roster, 6), "args"), "9: the whole roster was accepted as one share of three");
    CHECK(slate_refused(slate_dag_lens(b, NULL, 0), "args"), "9: a container carrying a roster was left share-less");
    CHECK(slate_dag_lens(b, share0, 2) == NULL, "9: share 0 was refused");
    CHECK(slate_dag_lens(b, share1, 2) == NULL, "9: share 1 was refused");
    /* and a split the pinned share no longer fits is refused where it is named */
    CHECK(slate_refused(slate_dag_shares(b, 2), "args"), "9: a split that leaves the pinned primes astride it was accepted");
    CHECK(slate_dag_shares(b, 3) == NULL, "9: the roster's own split was refused");

    /* a run with a roster set names its construction: the ask carries a program word and the dims */
    uint32_t c = slate_dag_carrier(b, NULL, 0);
    const int64_t vals[4] = { 11, 12, 13, 14 };
    slate_dag_carrier_set(b, c, vals, 4);
    int32_t root = slate_dag_add(b, slate_dag_load(b, c, slate_dag_param(b, 0)), slate_dag_lit(b, 7));
    const int64_t dims[1] = { 4 };
    SlateArray *arr = slate_dag_run(b, root, dims, 1);
    CHECK(arr != NULL, "9: the run over a share produced no reading");

    uint8_t *ask = NULL; uint64_t askn = 0;
    CHECK(slate_dag_ask(b, &ask, &askn) == 0 && ask != NULL, "9: the ask was refused");
    { uint8_t *nul = NULL; uint64_t nn = 0;
      CHECK(slate_dag_ask(NULL, &nul, &nn) != 0, "9: a null builder was asked");
      CHECK(slate_dag_ask(b, NULL, &nn) != 0, "9: a null out was accepted");
      CHECK(slate_dag_ask(b, &nul, NULL) != 0, "9: a null length was accepted"); }
    /* the ask reads back exactly what the container carries, in its one fixed order and nothing in front of
       it: shares, the roster, the dims, and the program's name as its two parts (the word, then the byte
       count of the leaf under it). The construction itself does not travel. */
    if (askn >= 8) {
      uint32_t shares_r = 0, k_r = 0;
      for (int i = 0; i < 4; i++) shares_r |= (uint32_t)ask[i] << (8 * i);
      for (int i = 0; i < 4; i++) k_r |= (uint32_t)ask[4 + i] << (8 * i);
      CHECK(shares_r == 3 && k_r == 6, "9: the ask says shares %u k %u (want 3, 6)", shares_r, k_r);
      uint64_t off = 8 + 8 * (uint64_t)k_r;
      uint32_t nd = 0; for (int i = 0; i < 4; i++) nd |= (uint32_t)ask[off + i] << (8 * i);
      CHECK(nd == 1, "9: the ask says %u dims (want 1)", nd);
      uint64_t d0 = 0; for (int i = 0; i < 8; i++) d0 |= (uint64_t)ask[off + 4 + i] << (8 * i);
      CHECK(d0 == 4, "9: the ask says dim %llu (want 4)", (unsigned long long)d0);
      uint64_t wn = 0; for (int i = 0; i < 8; i++) wn |= (uint64_t)ask[off + 12 + i] << (8 * i);
      CHECK(wn > 0, "9: the ask names no program — an API-built construction was not kept as a leaf");
      uint64_t pn = 0; for (int i = 0; i < 8 && off + 20 + wn + 8 <= askn; i++) pn |= (uint64_t)ask[off + 20 + wn + i] << (8 * i);
      CHECK(pn > 0, "9: the ask carries no byte count — the word alone does not say how much to read");
      CHECK(askn == off + 28 + wn, "9: the ask is %llu bytes, its own fields say %llu",
            (unsigned long long)askn, (unsigned long long)(off + 28 + wn));
    }

    /* the cell word: the reading's word with the cell's index after it */
    if (arr) {
      uint8_t *cw0 = NULL, *cw1 = NULL; uint64_t cn0 = 0, cn1 = 0;
      CHECK(slate_array_cell_word(arr, 0, &cw0, &cn0) == NULL && cn0 > 0, "9: cell 0 has no word");
      CHECK(slate_array_cell_word(arr, 1, &cw1, &cn1) == NULL && cn1 > 0, "9: cell 1 has no word");
      CHECK(cn0 != cn1 || memcmp(cw0, cw1, (size_t)cn0) != 0, "9: two cells of one reading share a word");
      CHECK(slate_array_cell_word(NULL, 0, &cw0, &cn0) != NULL, "9: a null reading was accepted");
      CHECK(slate_array_cell_word(arr, 0, NULL, &cn0) != NULL, "9: a null out was accepted");
      free(cw0); free(cw1);
    }

    /* the round trip: a fresh container takes the ask on a different share of the same roster (the runner
       carries share 1), and asks back the same bytes — the ask says what to run and over what lens, never who
       asked. Which share a machine carries is its own primes, handed in beside the ask. */
    SlateDag *u = slate_dag_new();
    slate_dag_codec(u, NULL, st_get, st_put, NULL, 0, &st);
    CHECK(slate_dag_take_ask(u, ask, askn, share0, 2) == NULL, "9: taking the ask on another share was refused");
    uint8_t *ask2 = NULL; uint64_t ask2n = 0;
    CHECK(slate_dag_ask(u, &ask2, &ask2n) == 0, "9: the taker's ask was refused");
    CHECK(ask2n == askn && ask2 && memcmp(ask, ask2, (size_t)askn) == 0,
          "9: the ask carries who asked — it did not round trip across shares (%llu vs %llu bytes)",
          (unsigned long long)ask2n, (unsigned long long)askn);
    /* the taker runs the ask's program on its own share, over the dims the ask carried — no dims named here */
    SlateArray *ua = slate_dag_start(u, NULL, 0);
    CHECK(ua != NULL, "9: the taker could not start the ask's program over the ask's dims");
    slate_array_free(ua);
    /* an ask whose share is not one of the roster's is refused, and so is a tag this build does not know */
    { SlateDag *x = slate_dag_new(); slate_dag_codec(x, NULL, st_get, st_put, NULL, 0, &st);
      CHECK(slate_refused(slate_dag_take_ask(x, ask, askn, astride, 2), "args"), "9: an ask taken astride the split");
      CHECK(slate_refused(slate_dag_take_ask(x, ask, askn, NULL, 0), "args"), "9: an ask taken with no share");
      uint8_t *bad = (uint8_t *)malloc((size_t)askn); memcpy(bad, ask, (size_t)askn);
      bad[4] = 0xFF; bad[5] = 0xFF; bad[6] = 0xFF; bad[7] = 0x0F;   /* a roster longer than the ask */
      CHECK(slate_refused(slate_dag_take_ask(x, bad, askn, share1, 2), "args"), "9: a roster past the ask's end was taken");
      memcpy(bad, ask, (size_t)askn);
      { uint64_t off = 8 + 8 * 6, zero = 0; memcpy(bad + off + 20 + 8, &zero, 8); }   /* a word with no count */
      CHECK(slate_refused(slate_dag_take_ask(x, bad, askn, share1, 2), "args"), "9: a word without its count was taken");
      CHECK(slate_refused(slate_dag_take_ask(x, ask, 4, share1, 2), "args"), "9: a truncated ask was taken");
      CHECK(slate_dag_take_ask(x, NULL, 0, share1, 2) != NULL, "9: a null ask was taken");
      free(bad); slate_dag_free(x); }

    free(ask); free(ask2);
    slate_array_free(arr);
    slate_dag_free(u); slate_dag_free(b); st_free(&st);
    printf("  roster: the whole lens and its split; a lens must be one share of it; the ask names the program and carries none of it, round trips, and the taker starts it on its own share; a cell has a word\n");
  }

  if (fails) { printf("FAIL test_abi_embed: %d checks failed\n", fails); return 1; }
  printf("PASS test_abi_embed: grant + host; the effect is a row; fail-closed; fibers overlap; search by shape and by computation; a pinned share is its own row and refuses a bad modulus; a program is a leaf, run by name; which leaf is context; a roster's share, its ask and its cell words\n");
  return 0;
}
