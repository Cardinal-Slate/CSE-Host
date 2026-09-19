/* The OS-effect door from pure C (slate/slate.h + slate/embed.h). No C++ in this translation unit. Installs a host vtable and
 * grants a capability, builds scalar effect nodes, runs them, and verifies: a granted effect resolves to the
 * host's exact value; identical requests collapse to one host call (the content-hash journal); distinct requests
 * each reach the host; and every un-granted path — capability withheld, wrong capability bit, no host installed —
 * refuses to a bottom without ever calling the host (fail-closed, never a wrong value). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"
#include "slate/embed.h"

/* A generic effect class is just a boundary channel, named; the bytes on it are protocol, built by a .slate
 * fragment. This test exercises the broker itself, so any name works. */
static const char *const SLATE_FX_GENERIC = "generic";
static const char *const SLATE_FX_CAPS[] = { "generic" };
static const char *const SLATE_FX_OTHER[] = { "other" };

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL effect_cabi: %s\n", msg); fails++; } } while (0)

/* A stand-in host: it ignores the request bytes and answers every effect with the same 8-byte little-endian
 * value, counting how many times it was actually reached. The engine frees the returned buffer. Returning 0 is
 * success; nonzero would fail the effect to a bottom. */
typedef struct { int calls; int64_t reply; } Host;

/* the store, outside the engine: a few (word, bytes) slots behind the region's decode/put callbacks. An effect's
   result is looked up by its word before the host runs, so an identical request is served from here. */
typedef struct { uint8_t word[64]; uint64_t wn; uint8_t *bytes; uint64_t n; } Slot;
typedef struct { Slot s[16]; int count; } Store;
static int store_get(const uint8_t *w, uint64_t wn, const uint8_t *sec, uint64_t sn, uint8_t **out, uint64_t *outn, void *u) {
  (void)sec; (void)sn; Store *st = (Store *)u;
  for (int i = 0; i < st->count; i++) if (st->s[i].wn == wn && memcmp(st->s[i].word, w, wn) == 0) {
    *out = (uint8_t *)malloc(st->s[i].n ? st->s[i].n : 1); memcpy(*out, st->s[i].bytes, st->s[i].n); *outn = st->s[i].n; return 0; }
  return 1;
}
static int store_put(const uint8_t *w, uint64_t wn, const uint8_t *b, uint64_t n, const uint8_t *sec, uint64_t sn, void *u) {
  (void)sec; (void)sn; Store *st = (Store *)u;
  if (st->count >= 16 || wn > 64) return 1;
  Slot *sl = &st->s[st->count++]; memcpy(sl->word, w, wn); sl->wn = wn; sl->bytes = (uint8_t *)malloc(n ? n : 1); memcpy(sl->bytes, b, n); sl->n = n;
  return 0;
}
static int host_perform(const char *cls, const uint8_t *req, uint64_t reqn,
                        uint8_t **out, uint64_t *outn, void *user) {
  Host *h = (Host *)user;
  (void)cls; (void)req; (void)reqn;
  h->calls++;
  uint8_t *b = (uint8_t *)malloc(8);
  if (!b) return 1;
  uint64_t u = (uint64_t)h->reply;
  for (int i = 0; i < 8; i++) b[i] = (uint8_t)(u >> (8 * i));
  *out = b; *outn = 8;
  return 0;
}

/* Dispatch `root` over a one-cell grid and read the single int64 value. Returns 0 and sets *v on an exact value,
 * or -1 if the dispatch refused (a bottom reading — a NULL result or a refused frame). */
static int run1(SlateDag *b, int32_t root, int64_t *v) {
  int64_t dims[1] = {1};
  SlateArray *a = slate_dag_run(b, root, dims, 1);
  if (!a) return -1;                                  /* refused dispatch: no result array */
  const slate_entry *re; int32_t rn;
  int refused = (slate_array_receipt(a, &re, &rn) == NULL && slate_entry_is(re, rn, "verdict", "undefined"));
  int64_t num[1], den[1];
  const char *rc = slate_array_i64_unsafe(a, num, den);
  slate_array_free(a);
  if (refused || rc != NULL) return -1;
  *v = num[0];
  return 0;
}

int main(void) {
  /* 1. a granted effect resolves to the host's value (a scalar effect node → an int64 leaf) */
  {
    Host h = {0, 42};
    SlateDag *b = slate_dag_new();
    Store st = {{{{0}, 0, NULL, 0}}, 0}; slate_dag_codec(b, NULL, store_get, store_put, NULL, 0, &st);   /* this block's own empty store */
    slate_dag_effect_caps(b, SLATE_FX_CAPS, 1);                    /* permit the class, by name */
    slate_dag_effect_host(b, host_perform, NULL, &h);
    int32_t e = slate_dag_effect(b, SLATE_FX_GENERIC, (const uint8_t *)"tick", 4, NULL, 0);
    CHECK(e >= 0, "effect node built");
    int64_t v = 0;
    CHECK(run1(b, e, &v) == 0, "granted effect ran");
    CHECK(v == 42, "granted effect resolved to the host value");
    CHECK(h.calls == 1, "host reached exactly once");
    slate_dag_free(b);
  }

  /* 2. the store: two effect nodes with an identical request — the second is served by its word, one host call */
  {
    Host h = {0, 21};
    Store st = {{{{0}, 0, NULL, 0}}, 0};
    SlateDag *b = slate_dag_new();
    slate_dag_effect_caps(b, SLATE_FX_CAPS, 1);
    slate_dag_effect_host(b, host_perform, NULL, &h);
    slate_dag_codec(b, NULL, store_get, store_put, NULL, 0, &st);
    int32_t e1 = slate_dag_effect(b, SLATE_FX_GENERIC, (const uint8_t *)"tick", 4, NULL, 0);
    int32_t e2 = slate_dag_effect(b, SLATE_FX_GENERIC, (const uint8_t *)"tick", 4, NULL, 0);
    int32_t root = slate_dag_add(b, e1, e2);
    int64_t v = 0;
    CHECK(run1(b, root, &v) == 0, "journaled effects ran");
    CHECK(v == 42, "both effects delivered the host value (21 + 21)");
    CHECK(h.calls == 1, "identical requests dedupe to one host call");
    slate_dag_free(b);
  }

  /* 3. distinct requests each reach the host (the journal keys on the request bytes, not the node). Each block below
   installs its own empty store: a shared region store answers a granted ask from a row an earlier block put, by design. */
  {
    Host h = {0, 21};
    SlateDag *b = slate_dag_new();
    Store st = {{{{0}, 0, NULL, 0}}, 0}; slate_dag_codec(b, NULL, store_get, store_put, NULL, 0, &st);   /* this block's own empty store */
    slate_dag_effect_caps(b, SLATE_FX_CAPS, 1);
    slate_dag_effect_host(b, host_perform, NULL, &h);
    int32_t e1 = slate_dag_effect(b, SLATE_FX_GENERIC, (const uint8_t *)"tick", 4, NULL, 0);
    int32_t e2 = slate_dag_effect(b, SLATE_FX_GENERIC, (const uint8_t *)"tock", 4, NULL, 0);
    int32_t root = slate_dag_add(b, e1, e2);
    int64_t v = 0;
    CHECK(run1(b, root, &v) == 0, "distinct effects ran");
    CHECK(v == 42, "both effects delivered the host value");
    CHECK(h.calls == 2, "distinct requests each reach the host");
    slate_dag_free(b);
  }

  /* 4. capability withheld: no bit set → the effect refuses without reaching the host (no ambient authority) */
  {
    Host h = {0, 42};
    SlateDag *b = slate_dag_new();
    Store st = {{{{0}, 0, NULL, 0}}, 0}; slate_dag_codec(b, NULL, store_get, store_put, NULL, 0, &st);   /* this block's own empty store */
    slate_dag_effect_host(b, host_perform, NULL, &h);             /* host installed, but no caps granted */
    int32_t e = slate_dag_effect(b, SLATE_FX_GENERIC, (const uint8_t *)"tick", 4, NULL, 0);
    int64_t v = 0;
    CHECK(run1(b, e, &v) == -1, "un-permitted effect refuses");
    CHECK(h.calls == 0, "host never reached without the grant");
    slate_dag_free(b);
  }

  /* 5. a grant of some other class does not grant this one (a name is not ambient authority) */
  {
    Host h = {0, 42};
    SlateDag *b = slate_dag_new();
    Store st = {{{{0}, 0, NULL, 0}}, 0}; slate_dag_codec(b, NULL, store_get, store_put, NULL, 0, &st);   /* this block's own empty store */
    slate_dag_effect_caps(b, SLATE_FX_OTHER, 1);                  /* some other class, not "generic" */
    slate_dag_effect_host(b, host_perform, NULL, &h);
    int32_t e = slate_dag_effect(b, SLATE_FX_GENERIC, (const uint8_t *)"tick", 4, NULL, 0);
    int64_t v = 0;
    CHECK(run1(b, e, &v) == -1, "a grant of another class refuses");
    CHECK(h.calls == 0, "host never reached under another class's grant");
    slate_dag_free(b);
  }

  /* 6. capability granted but no host installed: still refuses, cleanly */
  {
    SlateDag *b = slate_dag_new();
    Store st = {{{{0}, 0, NULL, 0}}, 0}; slate_dag_codec(b, NULL, store_get, store_put, NULL, 0, &st);   /* this block's own empty store */
    slate_dag_effect_caps(b, SLATE_FX_CAPS, 1);
    int32_t e = slate_dag_effect(b, SLATE_FX_GENERIC, (const uint8_t *)"tick", 4, NULL, 0);
    int64_t v = 0;
    CHECK(run1(b, e, &v) == -1, "effect with no host refuses");
    slate_dag_free(b);
  }

  if (fails == 0) printf("PASS effect_cabi: all capability/journal/refusal checks\n");
  return fails ? 1 : 0;
}
