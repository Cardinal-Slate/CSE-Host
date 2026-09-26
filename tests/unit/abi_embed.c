/* tests/unit/abi_embed.c — the C ABI, embedder band (slate/embed.h), from pure C. No C++ in this translation unit.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 *
 * Gate: an effect fires only under a grant and an installed host, else it refuses to a bottom without reaching the
 * host; an effect's result is a row — the same request in another builder over the same store is served without
 * the host; the deadline gate aborts an effect to a clean refusal; async hosts park fibers, not threads — tasks
 * spawned in a scope overlap their waits and each reads its own value.
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
#include "slate/tune.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- a host: answers every effect with one 8-byte LE value, counting how often it is reached ---- */
typedef struct { int calls; int64_t reply; } Host;
static int host_perform(const char *cls, const uint8_t *req, uint64_t reqn, uint8_t **out, uint64_t *outn, void *u) {
  (void)cls; (void)req; (void)reqn; Host *h = (Host *)u; h->calls++;
  uint8_t *b = (uint8_t *)malloc(8); uint64_t v = (uint64_t)h->reply; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
  *out = b; *outn = 8; return 0;
}
/* ---- the store: rows behind decode/put. `partial` makes every get answer 2 — not whole: a door in front of
 * a store that gathered some carriers' shares of a word but not all. A reader must never read that as the end
 * of a leaf. ---- */
typedef struct { uint8_t *w; uint64_t wn; uint8_t *b; uint64_t n; } Row;
typedef struct { Row *r; size_t n, cap; long hits, puts; int partial; const uint8_t *pw; uint64_t pwn; } Store;
static int st_get_(const uint8_t *w, uint64_t wn, const uint8_t *s, uint64_t sn, uint8_t **out, uint64_t *outn, void *u) {
  (void)s; (void)sn; Store *st = (Store *)u;
  if (st->partial) return 2;
  if (st->pw && wn == st->pwn && memcmp(w, st->pw, (size_t)wn) == 0) return 2;   /* this one word: not whole */
  for (size_t i = 0; i < st->n; i++) if (st->r[i].wn == wn && memcmp(st->r[i].w, w, (size_t)wn) == 0) {
    st->hits++; *out = (uint8_t *)malloc(st->r[i].n ? (size_t)st->r[i].n : 1); memcpy(*out, st->r[i].b, (size_t)st->r[i].n); *outn = st->r[i].n; return 0; }
  return 1;
}
/* A leaf's cell is an ordinary row, read and written the way the engine lays one (tests/leaf_cells.h): a
 * piece of the leaf's bytes with a 1 above it, on the pool prefix the row's size says. */
#include "../leaf_cells.h"
static int st_put_(const uint8_t *w, uint64_t wn, const uint8_t *b, uint64_t n, const uint8_t *s, uint64_t sn, void *u) {
  (void)s; (void)sn; Store *st = (Store *)u; st->puts++;
  if (st->n == st->cap) { st->cap = st->cap ? st->cap * 2 : 64; st->r = (Row *)realloc(st->r, st->cap * sizeof(Row)); }
  Row *r = &st->r[st->n++]; r->w = (uint8_t *)malloc((size_t)wn); memcpy(r->w, w, (size_t)wn); r->wn = wn;
  r->b = (uint8_t *)malloc(n ? (size_t)n : 1); memcpy(r->b, b, (size_t)n); r->n = n; return 0;
}
/* the engine calls a store from several threads at once: one lock around the table */
static pthread_mutex_t st_mu = PTHREAD_MUTEX_INITIALIZER;
static int st_get(const uint8_t *w, uint64_t wn, const uint8_t *s, uint64_t sn, uint8_t **out, uint64_t *outn, void *u) {
  pthread_mutex_lock(&st_mu); const int rc = st_get_(w, wn, s, sn, out, outn, u); pthread_mutex_unlock(&st_mu); return rc;
}
static int st_put(const uint8_t *w, uint64_t wn, const uint8_t *b, uint64_t n, const uint8_t *s, uint64_t sn, void *u) {
  pthread_mutex_lock(&st_mu); const int rc = st_put_(w, wn, b, n, s, sn, u); pthread_mutex_unlock(&st_mu); return rc;
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

/* ---- async: begin hands the slot to a thread that completes it once every task has begun ----
   A task begins its host when its fiber parks on the miss, so "every task has begun" is "every task is parked
   at once". The completer waits for exactly that and then answers: the overlap is ordered, not timed. */
enum { kTasks = 4 };
static pthread_mutex_t g_begun_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_begun_cv = PTHREAD_COND_INITIALIZER;
static int g_begun = 0;
typedef struct { void *slot; int64_t reply; } Pending;
static void *completer(void *u) {
  Pending *p = (Pending *)u;
  pthread_mutex_lock(&g_begun_mu);
  while (g_begun < kTasks) pthread_cond_wait(&g_begun_cv, &g_begun_mu);
  pthread_mutex_unlock(&g_begun_mu);
  uint8_t d[8]; uint64_t v = (uint64_t)p->reply; for (int i = 0; i < 8; i++) d[i] = (uint8_t)(v >> (8 * i));
  slate_effect_complete(p->slot, d, 8); free(p); return NULL;
}
typedef struct { int begins; int64_t reply; } AsyncHost;
static int host_begin(const char *cls, const uint8_t *req, uint64_t reqn, void *slot, void *u) {
  (void)cls; (void)req; (void)reqn; AsyncHost *h = (AsyncHost *)u; h->begins++;
  pthread_mutex_lock(&g_begun_mu); g_begun++; pthread_cond_broadcast(&g_begun_cv); pthread_mutex_unlock(&g_begun_mu);
  Pending *p = (Pending *)malloc(sizeof *p); p->slot = slot; p->reply = h->reply;
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
static void scope_body(SlateScope *s, void *u) { Task *ts = (Task *)u; for (int i = 0; i < kTasks; i++) slate_scope_spawn(s, task_run, &ts[i]); }

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
    Task ts[kTasks]; const char *blobs[kTasks] = { "a", "b", "c", "d" };
    for (int i = 0; i < kTasks; i++) { memset(&ts[i], 0, sizeof ts[i]); ts[i].host.reply = 100 + i; ts[i].rc = -1; ts[i].blob = blobs[i]; }
    slate_scope_run(scope_body, ts);
    for (int i = 0; i < kTasks; i++)
      CHECK(ts[i].rc == 0 && ts[i].got == 101 + i && ts[i].host.begins == 1,
            "4: task %d rc %d got %lld begins %d", i, ts[i].rc, (long long)ts[i].got, ts[i].host.begins);
    printf("  fibers: four tasks in one scope, all four parked at once on their own async hosts, each woke with its own value\n");
  }

  /* ---- 5. the lens: a share of the prime set is a different row, and a share never half-answers ---- */
  {
    Store st; memset(&st, 0, sizeof st);
    const int64_t a3[3] = { 1000003, 1000033, 1000037 };
    const int64_t b3[3] = { 1000039, 1000081, 1000099 };
    long put_unpinned = 0, put_a = 0, put_b = 0, put_a_again = 0;

    /* the same construction, three times: unpinned, then two disjoint shares, over one store */
    for (int pass = 0; pass < 4; pass++) {
      SlateDag *b = slate_dag_new();
      CHECK(b != NULL, "5: builder");
      slate_dag_codec(b, NULL, st_get, st_put, NULL, 0, &st);
      if (pass == 1 || pass == 3) CHECK(slate_dag_lens(b, a3, 3) == NULL, "5: pin a refused");
      if (pass == 2)              CHECK(slate_dag_lens(b, b3, 3) == NULL, "5: pin b refused");
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
      CHECK(arr != NULL, "5: pass %d produced no reading", pass);
      slate_array_free(arr);
      slate_dag_free(b);
    }
    /* each of the three named a row of its own: unpinned, share a, share b */
    CHECK(put_unpinned > 0, "5: the unpinned run kept nothing");
    CHECK(put_a > 0, "5: share a kept nothing — it was served the unpinned row");
    CHECK(put_b > 0, "5: share b kept nothing — it was served another share's row");
    /* and asking for share a a second time is a hit: the same share names the same row */
    CHECK(put_a_again == 0, "5: share a wrote %ld rows the second time — its name is not stable", put_a_again);
    printf("  lens: one construction, three prime sets, three rows; the same share twice is one row\n");

    /* the door refuses what would make a bad modulus, rather than accepting it */
    SlateDag *g = slate_dag_new();
    const int64_t too_big[1] = { (int64_t)1 << 24 };
    const int64_t dup[2] = { 1000003, 1000003 };
    const int64_t one[1] = { 1000003 };
    const int64_t nonprime[1] = { 1000001 };   /* 101 * 9901: composite, in range, not a repeat */
    CHECK(slate_dag_lens(g, too_big, 1) != NULL, "5: a prime past the lane's width was accepted");
    CHECK(slate_dag_lens(g, dup, 2) != NULL, "5: a repeated prime was accepted");
    CHECK(slate_dag_lens(g, NULL, 2) != NULL, "5: a null share with k>0 was accepted");
    CHECK(slate_dag_lens(NULL, one, 1) != NULL, "5: a null builder was accepted");
    CHECK(slate_refused(slate_dag_lens(g, nonprime, 1), "args"), "5: a non-prime was accepted");
    CHECK(slate_dag_lens(g, NULL, 0) == NULL, "5: unpinning was refused");
    slate_dag_free(g);
    printf("  lens: a prime past the lane's width, a repeat, a null share, and a non-prime are each refused\n");
  }

  /* ---- 6. a program is a leaf: emit names it, run and tail take the name, a name the store lacks refuses.
     A name is a word and nothing else — the reader walks the leaf's cells to the first one nobody has — so it
     is exactly a word wide. ---- */
  {
    Store st; memset(&st, 0, sizeof st);
    static const char *const CAP_SELF[] = { "slate.emit", "slate.run", "slate.tail" };
    int64_t v = 0;

    /* the width of a word over this codec, observed from the first row any dispatch keeps */
    SlateDag *b0 = slate_dag_new(); slate_dag_codec(b0, NULL, st_get, st_put, NULL, 0, &st);
    run1(b0, slate_dag_add(b0, slate_dag_lit(b0, 1), slate_dag_lit(b0, 1)), &v); slate_dag_free(b0);
    CHECK(st.n > 0, "6: no row to learn the word width from");
    const uint64_t wn = st.n ? st.r[0].wn : 0, nn = wn;   /* a name is a word, and nothing beside it */

    /* emit: the sub-DAG 20+22 becomes a leaf; the carrier holds its name, nothing else */
    SlateDag *b = slate_dag_new(); slate_dag_codec(b, NULL, st_get, st_put, NULL, 0, &st);
    slate_dag_effect_caps(b, CAP_SELF, 3);
    int32_t sub = slate_dag_add(b, slate_dag_lit(b, 20), slate_dag_lit(b, 22));
    uint32_t cw = slate_dag_effect_array(b, "slate.emit", NULL, 0, &sub, 1, (int64_t)nn);
    CHECK(cw != UINT32_MAX, "6: emit refused at build");
    /* run by name: the verdict carrier holds the program's reading as 8 LE byte-cells */
    uint32_t cv = slate_dag_effect_io(b, "slate.run", NULL, 0, &cw, 1, 8);
    CHECK(cv != UINT32_MAX, "6: run refused at build");
    long rows_before = (long)st.n;
    CHECK(run1(b, slate_dag_load(b, cv, slate_dag_lit(b, 0)), &v) == 0 && v == 42, "6: run by name read %lld (want 42)", (long long)v);
    CHECK((long)st.n > rows_before, "6: emit kept no row");
    /* the name itself, cell by cell, so another container can run the program with nothing but its name */
    uint8_t *word = (uint8_t *)calloc((size_t)nn, 1);
    int word_ok = 1;
    for (uint64_t i = 0; i < nn; i++) {
      if (run1(b, slate_dag_load(b, cw, slate_dag_lit(b, (int64_t)i)), &v) != 0) { word_ok = 0; break; }
      word[i] = (uint8_t)v;
    }
    CHECK(word_ok, "6: the emitted name could not be read back");
    slate_dag_free(b);

    /* a second container over the same store: the name alone runs the program — no bytes crossed */
    SlateDag *b2 = slate_dag_new(); slate_dag_codec(b2, NULL, st_get, st_put, NULL, 0, &st);
    slate_dag_effect_caps(b2, CAP_SELF, 3);
    uint32_t c2 = slate_dag_carrier_bytes(b2, word, nn);
    uint32_t cv2 = slate_dag_effect_io(b2, "slate.run", NULL, 0, &c2, 1, 8);
    CHECK(run1(b2, slate_dag_load(b2, cv2, slate_dag_lit(b2, 0)), &v) == 0 && v == 42,
          "6: a second container could not run the program by name (%lld)", (long long)v);
    /* the tail: hand off by name; the trampoline resolves it and the dispatch's reading is the program's */
    uint32_t ct = slate_dag_effect_io(b2, "slate.tail", NULL, 0, &c2, 1, 8);
    CHECK(run1(b2, slate_dag_load(b2, ct, slate_dag_lit(b2, 0)), &v) == 0 && v == 42,
          "6: the tail hand-off by name did not run the program (%lld)", (long long)v);
    slate_dag_free(b2);

    /* a name the store does not hold refuses: a program that was never kept does not exist */
    SlateDag *b3 = slate_dag_new(); slate_dag_codec(b3, NULL, st_get, st_put, NULL, 0, &st);
    slate_dag_effect_caps(b3, CAP_SELF, 3);
    uint8_t *junk = (uint8_t *)malloc((size_t)nn);       /* the same width, a word nothing was kept under */
    memcpy(junk, word, (size_t)nn);
    for (uint64_t i = 0; i < wn; i++) junk[i] = (uint8_t)(word[i] ^ 0xA5);
    uint32_t c3 = slate_dag_carrier_bytes(b3, junk, nn);
    uint32_t cv3 = slate_dag_effect_io(b3, "slate.run", NULL, 0, &c3, 1, 8);
    CHECK(run1(b3, slate_dag_load(b3, cv3, slate_dag_lit(b3, 0)), &v) != 0, "6: an unkept name ran (%lld)", (long long)v);
    uint32_t ct3 = slate_dag_effect_io(b3, "slate.tail", NULL, 0, &c3, 1, 8);
    CHECK(run1(b3, slate_dag_load(b3, ct3, slate_dag_lit(b3, 0)), &v) != 0, "6: a tail to an unkept name ran (%lld)", (long long)v);
    slate_dag_free(b3);
    free(word); free(junk); st_free(&st);
    printf("  program: emit keeps a leaf and yields its name; run and tail take the name, in this container or another; an unkept name refuses\n");
  }

  /* ---- 7. the program is context: set the name on the container, start it; no graph is built ---- */
  {
    Store st; memset(&st, 0, sizeof st);
    static const char *const CAP_SELF[] = { "slate.emit", "slate.run", "slate.tail" };
    int64_t v = 0;
    SlateDag *b0 = slate_dag_new(); slate_dag_codec(b0, NULL, st_get, st_put, NULL, 0, &st);
    run1(b0, slate_dag_add(b0, slate_dag_lit(b0, 1), slate_dag_lit(b0, 1)), &v); slate_dag_free(b0);
    const uint64_t wn = st.n ? st.r[0].wn : 0, nn = wn;   /* a name is a word, and nothing beside it */

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
    CHECK(word_ok, "7: the emitted name could not be read back");
    slate_dag_free(b);

    /* the entrypoint: a store, a word, start — the reading is the program's */
    const int64_t one = 1;
    SlateDag *e = slate_dag_new(); slate_dag_codec(e, NULL, st_get, st_put, NULL, 0, &st);
    CHECK(slate_dag_start(e, &one, 1) == NULL, "7: a container with no program started");
    CHECK(slate_dag_program(e, word, nn) == NULL, "7: setting the program refused");
    SlateArray *a = slate_dag_start(e, &one, 1);
    CHECK(a != NULL, "7: start refused a kept program");
    if (a) {
      int64_t num = 0, den = 1;
      CHECK(slate_array_i64_unsafe(a, &num, &den) == NULL && den == 1 && num == 42, "7: start read %lld (want 42)", (long long)num);
      slate_array_free(a);
    }
    /* clearing it, and a word the store does not hold, each refuse to start */
    CHECK(slate_dag_program(e, NULL, 0) == NULL, "7: clearing the program refused");
    CHECK(slate_dag_start(e, &one, 1) == NULL, "7: a cleared program started");
    uint8_t *junk = (uint8_t *)malloc((size_t)nn);
    memcpy(junk, word, (size_t)nn);
    for (uint64_t i = 0; i < wn; i++) junk[i] = (uint8_t)(word[i] ^ 0x5A);
    slate_dag_program(e, junk, nn);
    CHECK(slate_dag_start(e, &one, 1) == NULL, "7: an unkept program started");
    /* the word alone is the whole name: set it back and the container starts again */
    CHECK(slate_dag_program(e, word, nn) == NULL, "7: setting the program refused");
    { SlateArray *again = slate_dag_start(e, &one, 1);
      CHECK(again != NULL, "7: the word alone did not start the program");
      if (again) { int64_t num = 0, den = 1;
        CHECK(slate_array_i64_unsafe(again, &num, &den) == NULL && den == 1 && num == 42, "7: restart read %lld (want 42)", (long long)num);
        slate_array_free(again); } }
    CHECK(slate_dag_program(NULL, word, nn) != NULL, "7: a null builder was accepted");
    CHECK(slate_dag_program(e, NULL, 4) != NULL, "7: a null word with n > 0 was accepted");
    slate_dag_free(e);
    free(word); free(junk); st_free(&st);
    printf("  program: the leaf a container runs is context, named by its word alone; start runs it, and refuses with none, a cleared one, or an unkept one\n");
  }

  /* ---- 8. the roster, the share, the ask and the cell word: the doors a placement seam uses ---- */
  {
    Store st; memset(&st, 0, sizeof st);
    /* a roster of six primes. A share is a contiguous run of it — that is all the engine says, because every
       way of dividing a roster hands a unit one such run, and how many units there are is the deployment's
       rule, never a setting here and never in a word. */
    const int64_t roster[6] = { 1000003, 1000033, 1000037, 1000039, 1000081, 1000099 };
    const int64_t share0[2] = { 1000003, 1000033 };
    const int64_t share1[2] = { 1000037, 1000039 };
    const int64_t offcut[2] = { 1000033, 1000037 };    /* a run that straddles a cut into three — still a run */
    const int64_t gapped[2] = { 1000003, 1000037 };    /* two of the roster's primes with one skipped: no run */
    const int64_t reversed[2] = { 1000033, 1000003 };  /* the roster's own primes, out of its order: no run */
    const int64_t foreign[2] = { 1000117, 1000121 };   /* not in the roster at all */

    SlateDag *b = slate_dag_new();
    slate_dag_codec(b, NULL, st_get, st_put, NULL, 0, &st);
    CHECK(slate_dag_roster(b, roster, 6) == NULL, "8: the roster was refused");
    /* the roster takes the same prime rule as the lens */
    { const int64_t nonprime[1] = { 1000001 };
      CHECK(slate_refused(slate_dag_roster(b, nonprime, 1), "args"), "8: a non-prime roster was accepted");
      const int64_t dup2[2] = { 1000003, 1000003 };
      CHECK(slate_refused(slate_dag_roster(b, dup2, 2), "args"), "8: a repeated roster prime was accepted");
      CHECK(slate_dag_roster(NULL, roster, 6) != NULL, "8: a null builder was accepted");
      CHECK(slate_dag_roster(b, NULL, 3) != NULL, "8: a null roster with k>0 was accepted");
      CHECK(slate_dag_roster(b, roster, 6) == NULL, "8: the roster was refused on the way back"); }
    /* the lens must be a contiguous sub-range of it — and nothing else is a share */
    CHECK(slate_refused(slate_dag_lens(b, gapped, 2), "args"), "8: a lens with a gap in the roster was accepted");
    CHECK(slate_refused(slate_dag_lens(b, reversed, 2), "args"), "8: a lens out of the roster's order was accepted");
    CHECK(slate_refused(slate_dag_lens(b, foreign, 2), "args"), "8: a lens outside the roster was accepted");
    CHECK(slate_refused(slate_dag_lens(b, NULL, 0), "args"), "8: a container carrying a roster was left share-less");
    CHECK(slate_dag_lens(b, roster, 6) == NULL, "8: the whole roster was refused as a share (the undivided deployment)");
    CHECK(slate_dag_lens(b, offcut, 2) == NULL, "8: a run of the roster was refused because one division would cut it");
    CHECK(slate_dag_lens(b, share0, 2) == NULL, "8: share 0 was refused");
    CHECK(slate_dag_lens(b, share1, 2) == NULL, "8: share 1 was refused");
    /* and a roster the pinned share is no longer a run of is refused where it is named */
    { const int64_t other[3] = { 1000037, 1000081, 1000039 };
      CHECK(slate_refused(slate_dag_roster(b, other, 3), "args"), "8: a roster the pinned primes do not run in was accepted");
      CHECK(slate_dag_roster(b, roster, 6) == NULL, "8: the roster was refused on the way back"); }

    /* a run with a roster set names its construction: the ask carries a program word and the dims */
    uint32_t c = slate_dag_carrier(b, NULL, 0);
    const int64_t vals[4] = { 11, 12, 13, 14 };
    slate_dag_carrier_set(b, c, vals, 4);
    int32_t root = slate_dag_add(b, slate_dag_load(b, c, slate_dag_param(b, 0)), slate_dag_lit(b, 7));
    const int64_t dims[1] = { 4 };
    SlateArray *arr = slate_dag_run(b, root, dims, 1);
    CHECK(arr != NULL, "8: the run over a share produced no reading");

    /* the ask is a leaf like any other bytes: it goes through the door one cell per byte, and what comes back
       is its word. The bytes never cross a door. */
    const size_t before = st.n;
    uint8_t *ask = NULL; uint64_t askn = 0;
    CHECK(slate_dag_ask(b, &ask, &askn) == 0 && ask != NULL && askn > 0, "8: the ask was refused");
    { uint8_t *nul = NULL; uint64_t nn = 0;
      CHECK(slate_dag_ask(NULL, &nul, &nn) != 0, "8: a null builder was asked");
      CHECK(slate_dag_ask(b, NULL, &nn) != 0, "8: a null out was accepted");
      CHECK(slate_dag_ask(b, &nul, NULL) != 0, "8: a null length was accepted"); }
    /* nothing is kept under the bare word: a leaf lives in its cells, word ‖ 0, word ‖ 1, … */
    { uint8_t *o = NULL; uint64_t on = 0;
      CHECK(st_get(ask, askn, NULL, 0, &o, &on, &st) != 0, "8: the ask's bytes were kept under the bare word");
      free(o); }
    /* one cell per piece, in put order — the ask reads back off the rows the put left behind, and says exactly
       what the container carries, in its one fixed order with nothing in front of it: the roster, the dims,
       and the program's word. No division rides in front of the roster — that is the deployment's rule, not
       the ask's — and nothing rides beside the program's word: the taker walks the program's own leaf to the
       first cell nobody has. Neither construction travels. */
    const size_t ncells = st.n - before;
    unsigned char *bytes = (unsigned char *)malloc(ncells * LC_PIECE + 16);
    size_t nbytes = 0;
    int cells_ok = ncells >= 3;
    for (size_t i = 0; cells_ok && i < ncells; i++) {
      const int v = lc_cell_piece(st.r[before + i].b, st.r[before + i].n, bytes + nbytes);
      if (v < 0 || (i + 1 < ncells && v != LC_PIECE)) cells_ok = 0; else nbytes += (size_t)v;
    }
    CHECK(cells_ok, "8: the ask's leaf is not one cell per %d-byte piece (%zu rows)", LC_PIECE, ncells);
    if (cells_ok) {
      uint32_t k_r = 0;
      for (int i = 0; i < 4; i++) k_r |= (uint32_t)bytes[i] << (8 * i);
      CHECK(k_r == 6, "8: the ask says k %u (want 6) — the roster is the first field, nothing in front of it", k_r);
      size_t off = 4 + 8 * (size_t)k_r;
      cells_ok = k_r == 6 && nbytes >= off + 20;
      CHECK(cells_ok, "8: the ask's %zu bytes cannot hold its own fields", nbytes);
    }
    if (cells_ok) {
      const size_t off = 4 + 8 * (size_t)6;
      /* the roster itself, in its own order, right behind the count */
      for (int j = 0; j < 6; j++) {
        uint64_t p = 0; for (int i = 0; i < 8; i++) p |= (uint64_t)bytes[4 + 8 * (size_t)j + (size_t)i] << (8 * i);
        CHECK((int64_t)p == roster[j], "8: the ask's roster prime %d is %llu (want %lld)",
              j, (unsigned long long)p, (long long)roster[j]);
      }
      uint32_t nd = 0; for (int i = 0; i < 4; i++) nd |= (uint32_t)bytes[off + i] << (8 * i);
      CHECK(nd == 1, "8: the ask says %u dims (want 1)", nd);
      uint64_t d0 = 0; for (int i = 0; i < 8; i++) d0 |= (uint64_t)bytes[off + 4 + i] << (8 * i);
      CHECK(d0 == 4, "8: the ask says dim %llu (want 4)", (unsigned long long)d0);
      uint64_t pwn = 0; for (int i = 0; i < 8; i++) pwn |= (uint64_t)bytes[off + 12 + i] << (8 * i);
      CHECK(pwn > 0, "8: the ask names no program — an API-built construction was not kept as a leaf");
      CHECK(nbytes == off + 20 + (size_t)pwn, "8: the ask is %zu bytes, its own fields say %zu — something rides beside the word",
            nbytes, off + 20 + (size_t)pwn);
    }
    free(bytes);

    /* the reading's own word, and the word of one of its cells: the key the per-cell rows are kept under, and
       that key with the cell's index after it */
    if (arr) {
      uint8_t *rw = NULL, *cw0 = NULL, *cw1 = NULL; uint64_t rn = 0, cn0 = 0, cn1 = 0;
      CHECK(slate_array_word(arr, &rw, &rn) == NULL && rn > 0, "8: the reading has no word");
      CHECK(slate_array_cell_word(arr, 0, &cw0, &cn0) == NULL && cn0 > 0, "8: cell 0 has no word");
      CHECK(slate_array_cell_word(arr, 1, &cw1, &cn1) == NULL && cn1 > 0, "8: cell 1 has no word");
      CHECK(cn0 != cn1 || memcmp(cw0, cw1, (size_t)cn0) != 0, "8: two cells of one reading share a word");
      CHECK(rn != cn0 || memcmp(rw, cw0, (size_t)rn) != 0, "8: the reading's word is cell 0's word");
      CHECK(slate_array_word(NULL, &rw, &rn) != NULL, "8: a null reading was accepted");
      CHECK(slate_array_word(arr, NULL, &rn) != NULL, "8: a null out was accepted");
      CHECK(slate_array_cell_word(NULL, 0, &cw0, &cn0) != NULL, "8: a null reading was accepted");
      CHECK(slate_array_cell_word(arr, 0, NULL, &cn0) != NULL, "8: a null out was accepted");
      free(rw); free(cw0); free(cw1);
    }

    /* the round trip: a fresh container takes the ask by its word on a different share of the same roster (the
       runner carries share 0), and asks back the same word — the ask says what to run and over what lens,
       never who asked, so the same context is the same leaf and the same name, and nothing is kept twice.
       Which share a machine carries is its own primes, handed in beside the word. */
    SlateDag *u = slate_dag_new();
    slate_dag_codec(u, NULL, st_get, st_put, NULL, 0, &st);
    CHECK(slate_dag_take_ask(u, ask, askn, share0, 2) == NULL, "8: taking the ask on another share was refused");
    const size_t rows_taken = st.n;
    uint8_t *ask2 = NULL; uint64_t ask2n = 0;
    CHECK(slate_dag_ask(u, &ask2, &ask2n) == 0, "8: the taker's ask was refused");
    CHECK(ask2n == askn && ask2 && memcmp(ask, ask2, (size_t)askn) == 0,
          "8: the ask carries who asked — it did not round trip across shares (%llu vs %llu bytes)",
          (unsigned long long)ask2n, (unsigned long long)askn);
    CHECK(st.n == rows_taken, "8: the same ask was kept twice (%zu rows became %zu)", rows_taken, st.n);
    /* the taker runs the ask's program on its own share, over the dims the ask carried — no dims named here */
    SlateArray *ua = slate_dag_start(u, NULL, 0);
    CHECK(ua != NULL, "8: the taker could not start the ask's program over the ask's dims");
    slate_array_free(ua);
    /* run_ask: one call is a unit's whole answer to an Interest for an ask's word — take it, start it, hand
       back the ROOT's word. Over this store every row is local, so the root is whole and the refusal is NULL. */
    { SlateDag *r = slate_dag_new(); slate_dag_codec(r, NULL, st_get, st_put, NULL, 0, &st);
      uint8_t *root = NULL; uint64_t rn = 0;
      const char *rc = slate_dag_run_ask(r, ask, askn, share1, 2, &root, &rn);
      CHECK(rc == NULL, "8: running the ask by its word said \"%s\" (want whole)", rc ? rc : "");
      CHECK(root != NULL && rn > 0, "8: a run of the ask handed back no root word");
      /* the root's word is a name, not a reading: a second run over the same store names the same root */
      SlateDag *r2 = slate_dag_new(); slate_dag_codec(r2, NULL, st_get, st_put, NULL, 0, &st);
      uint8_t *root2 = NULL; uint64_t rn2 = 0;
      CHECK(slate_dag_run_ask(r2, ask, askn, share1, 2, &root2, &rn2) == NULL, "8: the second run of the ask refused");
      CHECK(root2 && rn2 == rn && memcmp(root, root2, (size_t)rn) == 0, "8: two runs of one ask named two roots");
      free(root); free(root2); slate_dag_free(r); slate_dag_free(r2); }
    /* the stopped state: a door that answers "not whole" for the root's first cell — this unit's share is
       there, another carrier's is not — leaves the unit at its share and says so, with the root's word all
       the same, so whoever lands the last share can still wake it by that name. */
    if (arr) {
      uint8_t *cw = NULL; uint64_t cn = 0;
      if (slate_array_cell_word(arr, 0, &cw, &cn) == NULL) {
        SlateDag *s = slate_dag_new(); slate_dag_codec(s, NULL, st_get, st_put, NULL, 0, &st);
        uint8_t *root = NULL; uint64_t rn = 0;
        st.pw = cw; st.pwn = cn;
        const char *rc = slate_dag_run_ask(s, ask, askn, share1, 2, &root, &rn);
        st.pw = NULL; st.pwn = 0;
        CHECK(slate_refused(rc, "share"), "8: a root whose first cell is not whole said \"%s\"", rc ? rc : "whole");
        CHECK(root != NULL && rn > 0, "8: the stopped state handed back no root word");
        free(root); slate_dag_free(s);
      }
      free(cw);
    }
    /* a word that names no ask, and the null arguments */
    { SlateDag *r = slate_dag_new(); slate_dag_codec(r, NULL, st_get, st_put, NULL, 0, &st);
      uint8_t *root = NULL; uint64_t rn = 0;
      uint8_t *junk = (uint8_t *)malloc((size_t)askn); memcpy(junk, ask, (size_t)askn);
      for (uint64_t i = 0; i < askn; i++) junk[i] = (uint8_t)(ask[i] ^ 0x5A);
      CHECK(slate_refused(slate_dag_run_ask(r, junk, askn, share1, 2, &root, &rn), "args"), "8: a word naming no ask ran");
      CHECK(slate_dag_run_ask(NULL, ask, askn, share1, 2, &root, &rn) != NULL, "8: a null builder ran an ask");
      CHECK(slate_dag_run_ask(r, ask, askn, share1, 2, NULL, &rn) != NULL, "8: a null root was accepted");
      free(junk); slate_dag_free(r); }
    /* an ask whose share is not one of the roster's, a word nothing is kept under, a leaf the door could only
       half gather (not whole: never the end of a leaf), and the malformed asks */
    { SlateDag *x = slate_dag_new(); slate_dag_codec(x, NULL, st_get, st_put, NULL, 0, &st);
      CHECK(slate_refused(slate_dag_take_ask(x, ask, askn, gapped, 2), "args"), "8: an ask taken on a gap in its roster");
      CHECK(slate_refused(slate_dag_take_ask(x, ask, askn, foreign, 2), "args"), "8: an ask taken on primes outside its roster");
      CHECK(slate_refused(slate_dag_take_ask(x, ask, askn, NULL, 0), "args"), "8: an ask taken with no share");
      uint8_t *junk = (uint8_t *)malloc((size_t)askn); memcpy(junk, ask, (size_t)askn);
      for (uint64_t i = 0; i < askn; i++) junk[i] = (uint8_t)(ask[i] ^ 0xA5);
      CHECK(slate_refused(slate_dag_take_ask(x, junk, askn, share1, 2), "args"), "8: a word nothing is kept under was taken");
      CHECK(slate_dag_take_ask(x, NULL, 0, share1, 2) != NULL, "8: a null word was taken");
      st.partial = 1;
      CHECK(slate_refused(slate_dag_take_ask(x, ask, askn, share1, 2), "partial"), "8: a half-gathered ask was taken whole");
      st.partial = 0;
      free(junk); slate_dag_free(x); }
    /* a malformed ask, now that no bytes travel: the only thing that can lie is the store. Set the leaf's own
       first four bytes — the roster's count, the ask's very first field, in its first cell — to a roster longer than the leaf, and
       the reader, which bounds-checks every field against the end it walked to, refuses where it reads rather
       than guessing at the bytes. Then put them back. */
    if (cells_ok && ncells) {
      SlateDag *x = slate_dag_new(); slate_dag_codec(x, NULL, st_get, st_put, NULL, 0, &st);
      Row *c0 = &st.r[before];
      uint8_t *keep = (uint8_t *)malloc((size_t)c0->n), piece[16];
      memcpy(keep, c0->b, (size_t)c0->n);
      const int pl = lc_cell_piece(c0->b, c0->n, piece);
      CHECK(pl >= 4, "8: the ask's first cell holds no roster count");
      const unsigned char huge[4] = { 0xFF, 0xFF, 0xFF, 0x0F };
      memcpy(piece, huge, 4);
      lc_cell_make(piece, pl, c0->b, c0->n);
      CHECK(slate_refused(slate_dag_take_ask(x, ask, askn, share1, 2), "args"), "8: a roster past the ask's end was taken");
      memcpy(c0->b, keep, (size_t)c0->n); free(keep);
      CHECK(slate_dag_take_ask(x, ask, askn, share1, 2) == NULL, "8: the ask put back was refused");
      slate_dag_free(x);
    }

    free(ask); free(ask2);
    slate_array_free(arr);
    slate_dag_free(u); slate_dag_free(b); st_free(&st);
    printf("  roster: the whole lens, with no division in it; a lens must be a contiguous run of it; the ask is a leaf named by its word, round trips, and running it by that word hands back the root's word\n");
  }

  /* ---- 9. the work a container declares bounds its ticks: a program that tails into itself stops Incomplete when
     its declared work is spent — no clock anywhere — and, run again with room, replays what it kept and closes: a
     tick over the same state is a hit, tail and all, so a loop that changes nothing ends on its own ---- */
  {
    Store st; memset(&st, 0, sizeof st);
    static const char *const CAP_SELF[] = { "slate.emit", "slate.run", "slate.tail" };
    SlateDag *b = slate_dag_new(); slate_dag_codec(b, NULL, st_get, st_put, NULL, 0, &st);
    slate_dag_effect_caps(b, CAP_SELF, 3);
    uint32_t ct = slate_dag_effect_io(b, "slate.tail", NULL, 0, NULL, 0, 8);   /* no operand: a repeat tail */
    int32_t root = slate_dag_add(b, slate_dag_load(b, ct, slate_dag_lit(b, 0)), slate_dag_lit(b, 7));
    uint8_t *pw = NULL; uint64_t pn = 0;
    const char *se = slate_dag_save_fragment(b, root, NULL, 0, &pw, &pn);
    CHECK(se == NULL && pn > 0, "9: the program that tails into itself was not kept (%s)", se ? se : "no word");
    slate_dag_free(b);
    if (se == NULL && pn > 0) {
      const int64_t one = 1;
      SlateDag *e = slate_dag_new(); slate_dag_codec(e, NULL, st_get, st_put, NULL, 0, &st);
      slate_dag_effect_caps(e, CAP_SELF, 3);
      CHECK(slate_dag_work(e, 1) == NULL, "9: the work bound refused");
      CHECK(slate_dag_program(e, pw, pn) == NULL, "9: setting the program refused");
      SlateArray *a = slate_dag_start(e, &one, 1);
      CHECK(a != NULL, "9: a bounded loop handed back no reading");
      if (a) {
        const slate_entry *en; int32_t n = 0; slate_array_receipt(a, &en, &n);
        CHECK(slate_entry_is(en, n, "closure", "incomplete"), "9: the loop spent its one tick and did not say Incomplete");
        int64_t num = 0, den = 1;
        CHECK(slate_array_i64_unsafe(a, &num, &den) == NULL && num == 7 && den == 1, "9: the last tick read %lld (want 7)", (long long)num);
        slate_array_free(a);
      }
      /* again with room: the kept tick is a hit, the tail with it, and the run closes */
      CHECK(slate_dag_work(e, 0) == NULL, "9: clearing the work bound refused");
      a = slate_dag_start(e, &one, 1);
      CHECK(a != NULL, "9: the resumed loop handed back no reading");
      if (a) {
        const slate_entry *en; int32_t n = 0; slate_array_receipt(a, &en, &n);
        CHECK(slate_entry_is(en, n, "closure", "closed"), "9: the resumed loop did not close");
        int64_t num = 0, den = 1;
        CHECK(slate_array_i64_unsafe(a, &num, &den) == NULL && num == 7 && den == 1, "9: the resumed loop read %lld (want 7)", (long long)num);
        slate_array_free(a);
      }
      slate_dag_free(e);
    }
    free(pw); st_free(&st);
    printf("  work: a program that tails into itself stops Incomplete when its declared work is spent, and run again with room replays what it kept and closes\n");
  }

  /* ---- 10. an ask names its inputs: the program's operands, each by its word, bound to its holes in order ---- */
  {
    Store st; memset(&st, 0, sizeof st);
    SlateDag *b = slate_dag_new(); slate_dag_codec(b, NULL, st_get, st_put, NULL, 0, &st);
    const uint8_t zero3[3] = {0, 0, 0};
    const uint32_t h0 = slate_dag_carrier_bytes(b, zero3, 3), h1 = slate_dag_carrier_bytes(b, zero3, 3);
    const int32_t p = slate_dag_param(b, 0);
    const int32_t root = slate_dag_add(b, slate_dag_load(b, h0, p), slate_dag_mul(b, slate_dag_load(b, h1, p), slate_dag_lit(b, 10)));
    uint8_t *pw = NULL; uint64_t pwn = 0;
    const uint32_t holes[2] = { h0, h1 };
    CHECK(slate_dag_save_fragment(b, root, holes, 2, &pw, &pwn) == NULL, "10: the program was not kept");
    slate_dag_free(b);
    /* the asker, the way a browser asks: each input kept as a leaf, then the ask's bytes — no roster, the dims, the
       program's word, each input's word — kept as a leaf too; its word is the ask */
    SlateDag *a = slate_dag_new(); slate_dag_codec(a, NULL, st_get, st_put, NULL, 0, &st);
    const uint8_t x[3] = {1, 2, 3}, y[3] = {4, 5, 6}, ghost[32] = {7, 7, 7, 7, 7, 7, 7, 7};
    uint8_t *xw = NULL, *yw = NULL; uint64_t xwn = 0, ywn = 0;
    CHECK(slate_dag_leaf(a, x, 3, &xw, &xwn) == 0 && slate_dag_leaf(a, y, 3, &yw, &ywn) == 0, "10: the inputs were not kept");
    uint8_t buf[1024]; uint64_t bn = 0;
#define PUT32(v) do { uint32_t _v = (v); memcpy(buf + bn, &_v, 4); bn += 4; } while (0)
#define PUT64(v) do { uint64_t _v = (v); memcpy(buf + bn, &_v, 8); bn += 8; } while (0)
#define PUTW(w, n) do { PUT64(n); memcpy(buf + bn, (w), (size_t)(n)); bn += (n); } while (0)
    PUT32(0); PUT32(1); PUT64(3); PUTW(pw, pwn); PUTW(xw, xwn); PUTW(yw, ywn);
    uint8_t *ask = NULL; uint64_t askn = 0;
    CHECK(slate_dag_leaf(a, buf, bn, &ask, &askn) == 0, "10: the ask was not kept");
    /* the taker: the ask alone — the program and its inputs are walked back by their words */
    SlateDag *t = slate_dag_new(); slate_dag_codec(t, NULL, st_get, st_put, NULL, 0, &st);
    CHECK(slate_dag_take_ask(t, ask, askn, NULL, 0) == NULL, "10: the ask was not taken");
    SlateArray *r = slate_dag_start(t, NULL, 0);
    int64_t num[3] = {0}, den[3] = {0};
    CHECK(r && slate_array_i64_unsafe(r, num, den) == NULL && num[0] == 41 && num[1] == 52 && num[2] == 63,
          "10: the program over its inputs read %lld %lld %lld (want 41 52 63)", (long long)num[0], (long long)num[1], (long long)num[2]);
    slate_array_free(r);
    /* the taker says the same ask: its inputs ride with it */
    uint8_t *again = NULL; uint64_t againn = 0;
    CHECK(slate_dag_ask(t, &again, &againn) == 0 && againn == askn && !memcmp(again, ask, (size_t)askn), "10: the taker's ask is another ask");
    slate_dag_free(t);
    /* an input the store does not hold is not an input: the ask is not taken */
    bn = 0;
    PUT32(0); PUT32(1); PUT64(3); PUTW(pw, pwn); PUTW(xw, xwn); PUTW(ghost, sizeof ghost);
    uint8_t *ask3 = NULL; uint64_t ask3n = 0;
    CHECK(slate_dag_leaf(a, buf, bn, &ask3, &ask3n) == 0, "10: the second ask was not kept");
    SlateDag *v = slate_dag_new(); slate_dag_codec(v, NULL, st_get, st_put, NULL, 0, &st);
    CHECK(slate_refused(slate_dag_take_ask(v, ask3, ask3n, NULL, 0), "args"), "10: an ask naming an input the store does not hold was taken");
    /* a count that runs past the ask's end is no ask */
    bn = 0;
    PUT32(0); PUT32(1); PUT64(3); PUTW(pw, pwn); PUT64(~(uint64_t)0);
    uint8_t *ask4 = NULL; uint64_t ask4n = 0;
    CHECK(slate_dag_leaf(a, buf, bn, &ask4, &ask4n) == 0 && slate_refused(slate_dag_take_ask(v, ask4, ask4n, NULL, 0), "args"),
          "10: an input count past the ask's end was taken");
#undef PUT32
#undef PUT64
#undef PUTW
    slate_dag_free(a);
    /* bytes read back are the bytes kept: the mirror door */
    { SlateDag *m = slate_dag_new(); slate_dag_codec(m, NULL, st_get, st_put, NULL, 0, &st);
      const uint8_t text[] = "bytes handed in, read back whole";
      uint8_t *w = NULL, *back = NULL; uint64_t wn = 0, bn = 0;
      CHECK(slate_dag_leaf(m, text, sizeof text - 1, &w, &wn) == 0 && slate_dag_leaf_read(m, w, wn, &back, &bn) == NULL &&
            bn == sizeof text - 1 && !memcmp(back, text, (size_t)bn), "10: bytes read back were not the bytes kept");
      CHECK(slate_refused(slate_dag_leaf_read(m, (const uint8_t *)"nothing", 7, &back, &bn), "args") || bn == 0, "10: a word nobody kept read as bytes");
      free(w); free(back); slate_dag_free(m); }
    slate_dag_free(v);
    free(pw); free(xw); free(yw); free(ask); free(again); free(ask3); free(ask4); st_free(&st);
    printf("  inputs: an ask names its program's inputs by their words; the taker walks them back and binds them to the holes; an input the store lacks is no ask\n");
  }

  if (fails) { printf("FAIL test_abi_embed: %d checks failed\n", fails); return 1; }
  printf("PASS test_abi_embed: grant + host; the effect is a row; fail-closed; fibers overlap; a pinned share is its own row and refuses a bad modulus; a program is a leaf, run by name; which leaf is context; a roster's share is a run of it, its ask carries no division, and its cell words; the declared work bounds a program's ticks; an ask names its inputs by word; bytes read back are the bytes kept\n");
  return 0;
}
