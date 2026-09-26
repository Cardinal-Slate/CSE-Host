/* tests/memstore.h — a store on the seam, in memory, for the pure-C ABI tests. No C++ in a consumer.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 *
 * The three callbacks slate_dag_codec takes, over one table of (word -> bytes) rows: `ms_get` is the decode
 * (the word goes out, the row comes back, or a miss), `ms_put` is the put. Encode is left to the engine's own.
 * Rows are keyed by the whole word, hashed, so a test that keeps a program — a leaf of one row per piece —
 * stays linear rather than rescanning a list per cell.
 *
 * This is a test's store, not a cache in the engine: the engine holds nothing between calls, and every row a
 * test reads back was put here through the same door. Two builders handed the same table share a store, which
 * is how a program one container kept is loaded by another. */
#ifndef SLATE_TESTS_MEMSTORE_H
#define SLATE_TESTS_MEMSTORE_H

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct MsRow { uint8_t *w; uint64_t wn; uint8_t *b; uint64_t n; int hidden; struct MsRow *next; } MsRow;
/* `seq` is the rows in the order they were first put. A leaf is put one cell at a time, cell 0 first, so in a
   table that held nothing else the first n entries of `seq` are the n byte cells of that leaf, in order. */
typedef struct { MsRow **tab; size_t nbuckets; size_t rows; long hits, puts; MsRow **seq; size_t nseq, seqcap; } MemStore;

static uint64_t ms_hash(const uint8_t *w, uint64_t wn) {
  uint64_t h = 1469598103934665603ULL;
  for (uint64_t i = 0; i < wn; i++) { h ^= w[i]; h *= 1099511628211ULL; }
  return h;
}
static void ms_init(MemStore *s, size_t nbuckets) {
  memset(s, 0, sizeof *s);
  s->nbuckets = nbuckets ? nbuckets : 4096;
  s->tab = (MsRow **)calloc(s->nbuckets, sizeof(MsRow *));
}
static MsRow *ms_find(MemStore *s, const uint8_t *w, uint64_t wn) {
  if (!s->tab) return NULL;
  for (MsRow *r = s->tab[ms_hash(w, wn) % s->nbuckets]; r; r = r->next)
    if (r->wn == wn && memcmp(r->w, w, (size_t)wn) == 0) return r->hidden ? NULL : r;
  return NULL;
}
static int ms_get_(const uint8_t *w, uint64_t wn, const uint8_t *sec, uint64_t sn,
                  uint8_t **out, uint64_t *outn, void *user) {
  (void)sec; (void)sn;
  MemStore *s = (MemStore *)user;
  MsRow *r = ms_find(s, w, wn);
  if (!r) return 1;                                   /* not there: a miss, never a guess */
  s->hits++;
  *out = (uint8_t *)malloc(r->n ? (size_t)r->n : 1);
  if (!*out) return 1;
  memcpy(*out, r->b, (size_t)r->n);
  *outn = r->n;
  return 0;
}
static int ms_put_(const uint8_t *w, uint64_t wn, const uint8_t *b, uint64_t n,
                  const uint8_t *sec, uint64_t sn, void *user) {
  (void)sec; (void)sn;
  MemStore *s = (MemStore *)user;
  if (!s->tab) return 1;
  s->puts++;
  MsRow *r = ms_find(s, w, wn);
  if (r) { free(r->b); r->b = (uint8_t *)malloc(n ? (size_t)n : 1); if (!r->b) return 1; memcpy(r->b, b, (size_t)n); r->n = n; return 0; }
  r = (MsRow *)calloc(1, sizeof *r);
  if (!r) return 1;
  r->w = (uint8_t *)malloc((size_t)(wn ? wn : 1)); memcpy(r->w, w, (size_t)wn); r->wn = wn;
  r->b = (uint8_t *)malloc(n ? (size_t)n : 1); memcpy(r->b, b, (size_t)n); r->n = n;
  size_t h = (size_t)(ms_hash(w, wn) % s->nbuckets);
  r->next = s->tab[h]; s->tab[h] = r; s->rows++;
  if (s->nseq == s->seqcap) { s->seqcap = s->seqcap ? s->seqcap * 2 : 1024; s->seq = (MsRow **)realloc(s->seq, s->seqcap * sizeof(MsRow *)); }
  s->seq[s->nseq++] = r;                              /* put order: a leaf's cells, cell 0 first (a leaf this small
                                                         is kept on one thread) */
  return 0;
}
/* the engine calls a store from several threads at once: one lock around the table */
static pthread_mutex_t ms_mu = PTHREAD_MUTEX_INITIALIZER;
static int ms_get(const uint8_t *w, uint64_t wn, const uint8_t *s, uint64_t sn, uint8_t **out, uint64_t *outn, void *u) {
  pthread_mutex_lock(&ms_mu); const int rc = ms_get_(w, wn, s, sn, out, outn, u); pthread_mutex_unlock(&ms_mu); return rc;
}
static int ms_put(const uint8_t *w, uint64_t wn, const uint8_t *b, uint64_t n, const uint8_t *s, uint64_t sn, void *u) {
  pthread_mutex_lock(&ms_mu); const int rc = ms_put_(w, wn, b, n, s, sn, u); pthread_mutex_unlock(&ms_mu); return rc;
}
static void ms_free(MemStore *s) {
  for (size_t i = 0; s->tab && i < s->nbuckets; i++)
    for (MsRow *r = s->tab[i], *nx; r; r = nx) { nx = r->next; free(r->w); free(r->b); free(r); }
  free(s->tab); free(s->seq);
  memset(s, 0, sizeof *s);
}

/* ---- a leaf's cells, as rows ------------------------------------------------------------------------------
 *
 * A leaf's cell is an ordinary row holding one piece of its bytes (tests/leaf_cells.h reads and writes one the
 * way the engine lays it). In a table that holds one leaf and nothing else, the rows in put order are that
 * leaf's cells, cell 0 first. This is how a store lies to a load: it is the only thing that can, now that no
 * file carries the bytes. */
#include "leaf_cells.h"
/* How many cells a walk over this table's leaf finds: the rows in put order, while each is a piece nobody has
   hidden. Nothing recorded it. */
static size_t ms_leaf_cells(const MemStore *s) {
  size_t n = 0; uint8_t p[16];
  while (n < s->nseq && !s->seq[n]->hidden && lc_cell_piece(s->seq[n]->b, s->seq[n]->n, p) >= 0) n++;
  return n;
}
/* How many bytes those cells hold: the leaf's length, as a walk reads it. */
static size_t ms_leaf_len(const MemStore *s) {
  size_t n = 0, c = ms_leaf_cells(s); uint8_t p[16];
  for (size_t i = 0; i < c; i++) n += (size_t)lc_cell_piece(s->seq[i]->b, s->seq[i]->n, p);
  return n;
}
/* The first `n` bytes of this table's leaf. 0 when its cells do not hold that many. */
static int ms_leaf_read(const MemStore *s, size_t n, unsigned char *out) {
  size_t got = 0, c = ms_leaf_cells(s); uint8_t p[16];
  for (size_t i = 0; i < c && got < n; i++) {
    const int v = lc_cell_piece(s->seq[i]->b, s->seq[i]->n, p);
    const size_t take = (size_t)v < n - got ? (size_t)v : n - got;
    memcpy(out + got, p, take); got += take;
  }
  return got == n;
}
/* Make this table's leaf hold these bytes — a store that hands back something else: cell i holds the i-th piece,
   on the row it already is. 0 when the table has too few cells for them. */
static int ms_leaf_write(MemStore *s, const unsigned char *in, size_t n) {
  const size_t cells = (n + LC_PIECE - 1) / LC_PIECE;
  if (cells > s->nseq) return 0;
  for (size_t i = 0; i < cells; i++) {
    const size_t lo = i * LC_PIECE, w = n - lo < LC_PIECE ? n - lo : LC_PIECE;
    lc_cell_make(in + lo, (int)w, s->seq[i]->b, s->seq[i]->n);
  }
  return 1;
}
/* Make a walk over this table's leaf stop after `n` cells: every later cell becomes one nobody has. A hidden
   row is still in the table; `ms_find` simply does not answer with it, which is exactly a miss. This is the
   only thing that can shorten a leaf now, because nothing anywhere records how long one is — the records are
   the count. Call with a large `n` to put every cell back. */
static void ms_leaf_stop_cells(MemStore *s, size_t n) {
  for (size_t i = 0; i < s->nseq; i++) s->seq[i]->hidden = (i >= n);
}
/* The same, said in bytes: keep the cells that hold the first `n` bytes. */
static void ms_leaf_stop_after(MemStore *s, size_t n) { ms_leaf_stop_cells(s, (n + LC_PIECE - 1) / LC_PIECE); }
/* Hand a builder this table as its store. Encode is the engine's own (a hash of the bytes). */
#define MS_INSTALL(b, s) slate_dag_codec((b), NULL, ms_get, ms_put, NULL, 0, (s))

/* ---- a program, as the fragment doors name one: the word of its leaf, and nothing beside it ----
 *
 * The tests below keep and load programs through one shared table, which is what a store is: a program one
 * container kept is a program another container over the same store can load, by its word and never by bytes.
 * `ms_dag` hands out a container over that table; `ms_keep` keeps a construction and names it; `ms_load` walks
 * the leaf back (in a container of its own — a loaded fragment is independent of the builder that read it). */
typedef struct { uint8_t *w; uint64_t wn; } MsProg;

static MemStore ms_store;
static void ms_store_open(void) { if (!ms_store.tab) ms_init(&ms_store, 1 << 14); }
static SlateDag *ms_dag(void) {
  ms_store_open();
  SlateDag *b = slate_dag_new();
  if (b) MS_INSTALL(b, &ms_store);
  return b;
}
static const char *ms_keep(SlateDag *b, int32_t root, const uint32_t *holes, uint32_t nholes, MsProg *out) {
  out->w = NULL; out->wn = 0;
  return slate_dag_save_fragment(b, root, holes, nholes, &out->w, &out->wn);
}
static SlateFrag *ms_load(const MsProg *p) {
  if (!p || !p->w) return NULL;
  SlateDag *b = ms_dag();
  if (!b) return NULL;
  SlateFrag *f = slate_frag_load(b, p->w, p->wn);
  slate_dag_free(b);                 /* the fragment is its own thing once parsed */
  return f;
}
static void ms_prog_free(MsProg *p) { if (p) { free(p->w); p->w = NULL; p->wn = 0; } }

#endif
