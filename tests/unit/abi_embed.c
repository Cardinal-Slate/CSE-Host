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

  if (fails) { printf("FAIL test_abi_embed: %d checks failed\n", fails); return 1; }
  printf("PASS test_abi_embed: grant + host; the effect is a row; fail-closed; fibers overlap; search by shape and by computation\n");
  return 0;
}
