/* The consume door from pure C: a cursor is the word of the chunk just read, never a position. A host serves a
 * 10-byte resource; three consumes walk it by presenting each chunk's word to the next; a fresh builder sharing the
 * store resumes from a word without the host being asked for the chunk it names; a word no consume produced refuses.
 * No C++ in this translation unit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"
#include "slate/embed.h"

static const char *const CLS = "feed";
static const char *const CAPS[] = { "feed" };
static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL consume_cabi: %s\n", msg); fails++; } } while (0)

/* the store, outside the engine */
typedef struct { uint8_t word[64]; uint64_t wn; uint8_t *bytes; uint64_t n; } Slot;
typedef struct { Slot s[32]; int count; } Store;
static int store_get(const uint8_t *w, uint64_t wn, const uint8_t *sec, uint64_t sn, uint8_t **out, uint64_t *outn, void *u) {
  (void)sec; (void)sn; Store *st = (Store *)u;
  for (int i = 0; i < st->count; i++) if (st->s[i].wn == wn && memcmp(st->s[i].word, w, wn) == 0) {
    *out = (uint8_t *)malloc(st->s[i].n ? st->s[i].n : 1); memcpy(*out, st->s[i].bytes, st->s[i].n); *outn = st->s[i].n; return 0; }
  return 1;
}
static int store_put(const uint8_t *w, uint64_t wn, const uint8_t *b, uint64_t n, const uint8_t *sec, uint64_t sn, void *u) {
  (void)sec; (void)sn; Store *st = (Store *)u;
  if (st->count >= 32 || wn > 64) return 1;
  Slot *sl = &st->s[st->count++]; memcpy(sl->word, w, wn); sl->wn = wn; sl->bytes = (uint8_t *)malloc(n ? n : 1); memcpy(sl->bytes, b, n); sl->n = n;
  return 0;
}

/* the host: a 10-byte resource. The request is class | blob | nargs=2 | offset, len (each: i32 sign, u32 nlimbs,
 * u64 limb, u32 dlimbs, u64 den). It answers the bytes at [offset, offset+len). */
typedef struct { int calls; uint64_t last_offset; } Host;
static const char RES[10] = "0123456789";
static int host_perform(const char *cls, const uint8_t *req, uint64_t reqn, uint8_t **out, uint64_t *outn, void *user) {
  Host *h = (Host *)user;
  if (strcmp(cls, CLS) != 0 || reqn < 8) return 1;
  uint32_t clen, blen, nargs; memcpy(&clen, req, 4); memcpy(&blen, req + 4 + clen, 4);
  const uint8_t *p = req + 8 + clen + blen; memcpy(&nargs, p, 4); p += 4;
  if (nargs != 2) return 1;
  uint64_t v[2];
  for (int i = 0; i < 2; i++) { memcpy(&v[i], p + 8, 8); p += 4 + 4 + 8 + 4 + 8; }   /* sign, nlimbs, limb, dlimbs, den */
  uint64_t off = v[0], len = v[1];
  h->calls++; h->last_offset = off;
  uint64_t avail = off < 10 ? 10 - off : 0, take = len < avail ? len : avail;
  uint8_t *b = (uint8_t *)malloc(take ? take : 1); memcpy(b, RES + off, take);
  *out = b; *outn = take;
  return 0;
}

/* read n cells of carrier cid: root = load(cid, param 0) over dims {n}; -1 if the run refused */
static int read_cells(SlateDag *b, uint32_t cid, int64_t n, int64_t *out) {
  int32_t root = slate_dag_load(b, cid, slate_dag_param(b, 0));
  int64_t dims[1] = { n };
  SlateArray *a = slate_dag_run(b, root, dims, 1);
  if (!a) return -1;
  const slate_entry *re; int32_t rn;
  int refused = (slate_array_receipt(a, &re, &rn) == NULL && slate_entry_is(re, rn, "verdict", "undefined"));
  int64_t *den = (int64_t *)malloc((size_t)n * 8);
  const char *rc = slate_array_i64_unsafe(a, out, den); free(den); slate_array_free(a);
  return (refused || rc != NULL) ? -1 : 0;
}

/* one consume in a fresh builder over the shared store: returns 0 and fills cells + the cursor, or -1 */
static int consume_once(Store *st, Host *h, const uint8_t *cursor, uint64_t cn, int64_t *cells, uint8_t *word_out, uint64_t *wn_out, char *closure_out) {
  SlateDag *b = slate_dag_new();
  slate_dag_effect_caps(b, CAPS, 1);
  slate_dag_effect_host(b, host_perform, NULL, h);
  slate_dag_codec(b, NULL, store_get, store_put, NULL, 0, st);
  uint32_t cid = slate_dag_consume(b, CLS, (const uint8_t *)"res", 3, cursor, cn, 4);
  int rc = read_cells(b, cid, 4, cells);
  if (rc == 0) {
    slate_cursor cu;
    if (slate_dag_cursor(b, cid, &cu) != NULL || cu.wn > 64) rc = -1;
    else { memcpy(word_out, cu.word, cu.wn); *wn_out = cu.wn; strncpy(closure_out, cu.closure, 15); closure_out[15] = 0; }
  }
  slate_dag_free(b);
  return rc;
}

int main(void) {
  Store st; memset(&st, 0, sizeof st);
  Host h = { 0, 0 };
  int64_t c[4]; uint8_t w1[64], w2[64], w3[64]; uint64_t n1, n2, n3; char cl[16];

  /* 1. from the start: "0123", incomplete, a word for the chunk */
  CHECK(consume_once(&st, &h, NULL, 0, c, w1, &n1, cl) == 0, "first consume ran");
  CHECK(c[0] == '0' && c[1] == '1' && c[2] == '2' && c[3] == '3', "first chunk bytes");
  CHECK(strcmp(cl, "incomplete") == 0 && n1 > 0, "first chunk: incomplete, with a word");
  CHECK(h.calls == 1 && h.last_offset == 0, "host asked once, at the start");

  /* 2. present the first chunk's word in a fresh builder: the position comes from the chunk's own row, not a number */
  CHECK(consume_once(&st, &h, w1, n1, c, w2, &n2, cl) == 0, "second consume ran");
  CHECK(c[0] == '4' && c[1] == '5' && c[2] == '6' && c[3] == '7', "second chunk bytes");
  CHECK(h.calls == 2 && h.last_offset == 4, "host asked once more, at 4, without re-reading the first chunk");
  CHECK(memcmp(w1, w2, n1 < n2 ? n1 : n2) != 0, "the two chunks have different words");

  /* 3. present the second: "89" and closed (a short read hit the end) */
  CHECK(consume_once(&st, &h, w2, n2, c, w3, &n3, cl) == 0, "third consume ran");
  CHECK(c[0] == '8' && c[1] == '9' && c[2] == 0 && c[3] == 0, "third chunk bytes, zero-padded");
  CHECK(strcmp(cl, "closed") == 0, "third chunk: closed");
  CHECK(h.calls == 3, "host asked for the third chunk");

  /* 4. the same ask again is a store hit: the host is not asked */
  CHECK(consume_once(&st, &h, w1, n1, c, w3, &n3, cl) == 0, "repeat consume ran");
  CHECK(c[0] == '4' && h.calls == 3, "the repeat was served from the store");

  /* 5. a word no consume produced refuses */
  uint8_t fake[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
  CHECK(consume_once(&st, &h, fake, 8, c, w3, &n3, cl) == -1, "an unknown cursor refuses");
  CHECK(h.calls == 3, "host never asked for an unknown cursor");

  if (fails == 0) printf("PASS consume_cabi: a cursor is the chunk's word; the next consume resolves its place from the chunk's row; unknown refuses\n");
  return fails ? 1 : 0;
}
