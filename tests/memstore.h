/* tests/memstore.h — a store on the seam, in memory, for the pure-C ABI tests. No C++ in a consumer.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 *
 * The three callbacks slate_dag_codec takes, over one table of (word -> bytes) rows: `ms_get` is the decode
 * (the word goes out, the row comes back, or a miss), `ms_put` is the put. Encode is left to the engine's own.
 * Rows are keyed by the whole word, hashed, so a test that keeps a program — a leaf of one row per byte —
 * stays linear rather than rescanning a list per cell.
 *
 * This is a test's store, not a cache in the engine: the engine holds nothing between calls, and every row a
 * test reads back was put here through the same door. Two builders handed the same table share a store, which
 * is how a program one container kept is loaded by another. */
#ifndef SLATE_TESTS_MEMSTORE_H
#define SLATE_TESTS_MEMSTORE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct MsRow { uint8_t *w; uint64_t wn; uint8_t *b; uint64_t n; struct MsRow *next; } MsRow;
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
    if (r->wn == wn && memcmp(r->w, w, (size_t)wn) == 0) return r;
  return NULL;
}
static int ms_get(const uint8_t *w, uint64_t wn, const uint8_t *sec, uint64_t sn,
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
static int ms_put(const uint8_t *w, uint64_t wn, const uint8_t *b, uint64_t n,
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
  s->seq[s->nseq++] = r;                              /* put order: a leaf's cells, cell 0 first */
  return 0;
}
static void ms_free(MemStore *s) {
  for (size_t i = 0; s->tab && i < s->nbuckets; i++)
    for (MsRow *r = s->tab[i], *nx; r; r = nx) { nx = r->next; free(r->w); free(r->b); free(r); }
  free(s->tab); free(s->seq);
  memset(s, 0, sizeof *s);
}

/* ---- a byte cell, as a row --------------------------------------------------------------------------------
 *
 * A leaf's cell is an ordinary row: [sign u8][num residue u32]*m [den residue u32]*m. `bytes_buffer` lays a
 * byte at width 1, so a byte over 127 is kept as its signed form: the sign byte is set and the magnitude is
 * 256 - v. Every prime of any lens is far larger than 256, so each residue of such a magnitude is just the
 * magnitude — which is why a test can read a cell's byte off its row, and write one into it, without knowing
 * a single prime. This is how a store lies to a load: it is the only thing that can, now that no file carries
 * the bytes. */
static int ms_cell_byte(const MsRow *r) {
  if (!r || r->n < 9 || (r->n - 1) % 8) return -1;
  uint32_t mag = 0; memcpy(&mag, r->b + 1, 4);
  if (r->b[0]) return (mag && mag <= 128) ? (int)(256u - mag) : -1;
  return mag <= 127 ? (int)mag : -1;
}
static void ms_cell_set(MsRow *r, unsigned char v) {
  if (!r || r->n < 9 || (r->n - 1) % 8) return;
  const uint32_t m = (uint32_t)((r->n - 1) / 8);
  const int neg = v >= 128;
  const uint32_t mag = neg ? (uint32_t)(256 - (int)v) : (uint32_t)v, one = 1;
  r->b[0] = (uint8_t)(neg ? 1 : 0);
  for (uint32_t j = 0; j < m; j++) { memcpy(r->b + 1 + 4 * j, &mag, 4); memcpy(r->b + 1 + 4 * (m + j), &one, 4); }
}
/* The first `n` byte cells of this table, as bytes. 0 when a cell is not a byte cell. */
static int ms_leaf_read(const MemStore *s, size_t n, unsigned char *out) {
  if (s->nseq < n) return 0;
  for (size_t i = 0; i < n; i++) { int v = ms_cell_byte(s->seq[i]); if (v < 0) return 0; out[i] = (unsigned char)v; }
  return 1;
}
/* Make the first `n` byte cells of this table hold these bytes — a store that hands back something else. */
static void ms_leaf_write(MemStore *s, const unsigned char *in, size_t n) {
  for (size_t i = 0; i < n && i < s->nseq; i++) ms_cell_set(s->seq[i], in[i]);
}
/* Hand a builder this table as its store. Encode is the engine's own (a hash of the bytes). */
#define MS_INSTALL(b, s) slate_dag_codec((b), NULL, ms_get, ms_put, NULL, 0, (s))

/* ---- a program, as the fragment doors name one: the word of its leaf and the leaf's byte count ----
 *
 * The tests below keep and load programs through one shared table, which is what a store is: a program one
 * container kept is a program another container over the same store can load, by name and never by bytes.
 * `ms_dag` hands out a container over that table; `ms_keep` keeps a construction and names it; `ms_load` reads
 * the leaf back (in a container of its own — a loaded fragment is independent of the builder that read it). */
typedef struct { uint8_t *w; uint64_t wn, pn; } MsProg;

static MemStore ms_store;
static void ms_store_open(void) { if (!ms_store.tab) ms_init(&ms_store, 1 << 14); }
static SlateDag *ms_dag(void) {
  ms_store_open();
  SlateDag *b = slate_dag_new();
  if (b) MS_INSTALL(b, &ms_store);
  return b;
}
static const char *ms_keep(SlateDag *b, int32_t root, const uint32_t *holes, uint32_t nholes, MsProg *out) {
  out->w = NULL; out->wn = out->pn = 0;
  return slate_dag_save_fragment(b, root, holes, nholes, &out->w, &out->wn, &out->pn);
}
static SlateFrag *ms_load(const MsProg *p) {
  if (!p || !p->w) return NULL;
  SlateDag *b = ms_dag();
  if (!b) return NULL;
  SlateFrag *f = slate_frag_load(b, p->w, p->wn, p->pn);
  slate_dag_free(b);                 /* the fragment is its own thing once parsed */
  return f;
}
static void ms_prog_free(MsProg *p) { if (p) { free(p->w); p->w = NULL; p->wn = p->pn = 0; } }

#endif
