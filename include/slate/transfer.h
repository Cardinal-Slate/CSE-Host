/* Store-backed implementation selection, composed from the existing leaf, name and fragment doors.
 * SPDX-License-Identifier: MIT OR Apache-2.0 */
#ifndef SLATE_TRANSFER_H
#define SLATE_TRANSFER_H

#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"
#include "slate/embed.h"

/* Keep the construction first, validate the stored copy, then select its word under scope/name.
 * A failed write never intentionally publishes a new selection. The store's own durability/concurrency
 * contract still applies: this helper is not a transaction or a disk sync. */
static inline const char *slate_transfer_write(SlateDag *b,
    const uint8_t *scope, uint64_t sn, const uint8_t *name, uint64_t nn,
    const uint8_t *program, uint64_t n) {
  uint8_t *word = NULL; uint64_t wn = 0;
  if (!b || !scope || !sn || !name || !nn || !program || !n) return "args";
  if (slate_dag_leaf(b, program, n, &word, &wn)) return "write";
  SlateFrag *f = slate_frag_load(b, word, wn);
  if (!f) { free(word); return "program"; }
  slate_frag_free(f);
  const int rc = slate_dag_named_put(b, scope, sn, name, nn, word, wn);
  free(word);
  return rc ? "write" : NULL;
}

/* Read the selected program's word. Only "missing" permits a caller's legacy fallback. An empty
 * selection, a partial read, or a selected program that cannot run must not silently use the old code.
 * *word is malloc'd, freed by the caller. The existing named door reports an absent/unreadable entry
 * as "args"; the helper maps that to "missing" only after validating its own arguments. */
static inline const char *slate_transfer_read(SlateDag *b,
    const uint8_t *scope, uint64_t sn, const uint8_t *name, uint64_t nn,
    uint8_t **word, uint64_t *wn) {
  if (word) *word = NULL;
  if (wn) *wn = 0;
  if (!b || !scope || !sn || !name || !nn || !word || !wn) return "args";
  const char *rc = slate_dag_named_read(b, scope, sn, name, nn, word, wn);
  if (rc) return strcmp(rc, "args") == 0 ? "missing" : rc;
  if (!*wn) { free(*word); *word = NULL; return "program"; }
  return NULL;
}

/* Resolve the selection on every call (no process-local installed flag), then invoke the stored
 * program. The existing engine keeps/reuses computed results. Changing the selected program changes
 * the construction's identity. NULL means *answer is caller-owned; free with slate_array_free(). */
static inline const char *slate_transfer_invoke(SlateDag *b,
    const uint8_t *scope, uint64_t sn, const uint8_t *name, uint64_t nn,
    const uint8_t *const *args, const uint64_t *sizes, uint32_t nargs,
    const int64_t *dims, uint32_t ndims, SlateArray **answer) {
  uint8_t *word = NULL; uint64_t wn = 0;
  if (!answer || (nargs && (!args || !sizes))) return "args";
  *answer = NULL;
  for (uint32_t i = 0; i < nargs; i++) if (!args[i] && sizes[i]) return "args";
  const char *rc = slate_transfer_read(b, scope, sn, name, nn, &word, &wn);
  if (rc) return rc;
  *answer = slate_invoke(b, word, wn, args, sizes, nargs, NULL, 0, dims, ndims);
  free(word);
  return *answer ? NULL : "refused";
}

/* First migrated caller: (a*b) mod p for residues a,b < p, 2 <= p < 2^31. The construction is in
 * CSE-RNS/core/programs/rns/mul31.slate. allow_legacy=0 makes dependence on the old expression testable.
 * used_store is 1 only after a successful stored result, 0 for the legacy path (or a failure). This
 * does not replace slate_fold's internal multiplication or the engine that executes the construction. */
static inline const char *slate_transfer_mul31(SlateDag *b, const uint8_t *scope, uint64_t sn,
    uint64_t a, uint64_t c, uint64_t p, int allow_legacy, uint64_t *out, int *used_store) {
  static const uint8_t name[] = "rns.mul31";
  uint8_t input[12]; const uint8_t *args[] = {input}; const uint64_t sizes[] = {12};
  SlateArray *answer = NULL;
  if (used_store) *used_store = 0;
  if (!b || !scope || !sn || !out || p < 2 || p > 2147483647ULL || a >= p || c >= p) return "args";
  for (unsigned i = 0; i < 4; i++) {
    input[i] = (uint8_t)(a >> (8*i));
    input[4+i] = (uint8_t)(c >> (8*i));
    input[8+i] = (uint8_t)(p >> (8*i));
  }
  const char *rc = slate_transfer_invoke(b, scope, sn, name, sizeof name - 1,
                                       args, sizes, 1, NULL, 0, &answer);
  if (rc) {
    if (allow_legacy && strcmp(rc, "missing") == 0) { *out = (a*c) % p; return NULL; }
    return rc;
  }
  int64_t num = 0, den = 0;
  if (slate_array_size(answer) != 1) rc = "answer";
  else rc = slate_array_i64_unsafe(answer, &num, &den);
  slate_array_free(answer);
  if (rc) return rc;
  if (den != 1 || num < 0 || (uint64_t)num >= p) return "answer";
  *out = (uint64_t)num;
  if (used_store) *used_store = 1;
  return NULL;
}
#endif
