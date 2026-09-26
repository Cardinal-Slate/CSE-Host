/* tests/leaf_cells.h — a leaf's cell read and written from plain C, laid out the way the engine lays it.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 *
 * A leaf is its bytes cut into pieces, each piece one cell: an ordinary row, [sign u8][num residue u32]*m
 * [den residue u32]*m, sign 0, A = the piece with a 1 above its top byte, B = 1. Under a word that says no lens
 * — every word a test's store sees, since the engine's own encode hashes the record whole — the row sits on
 * the pool prefix its size says: the pool's first m-1 primes and the guard. The pool is the engine's
 * (wbuffer.hpp, rns_pool): the 1024 largest primes below 2^24, and the largest prime below 2^23 as the guard.
 *
 * At the default height a piece is 7 bytes (64 bits: the piece and the 1 above it), which the pool prefix
 * holds on three primes, so a cell fits 128 bits and Garner in unsigned __int128 reads it back. A test that
 * lies to a reader writes a cell this way; a test that checks what was kept reads one. */
#ifndef TESTS_LEAF_CELLS_H
#define TESTS_LEAF_CELLS_H

#include <stdint.h>
#include <string.h>

#define LC_PIECE 7          /* the bytes one piece carries at the default height */
#define LC_POOL 1024

static int64_t lc_pool_[LC_POOL], lc_guard_;
static int lc_ready_;

static int lc_is_prime(int64_t n) {
  if (n < 2) return 0;
  if (n % 2 == 0) return n == 2;
  for (int64_t d = 3; d * d <= n; d += 2) if (n % d == 0) return 0;
  return 1;
}
static void lc_init(void) {
  if (lc_ready_) return;
  int k = 0;
  for (int64_t c = ((int64_t)1 << 24) - 1; c > 2 && k < LC_POOL; c -= 2) if (lc_is_prime(c)) lc_pool_[k++] = c;
  for (int64_t c = ((int64_t)1 << 23) - 1; c > 2; c -= 2) if (lc_is_prime(c)) { lc_guard_ = c; break; }
  lc_ready_ = 1;
}
/* channel j of a row of m channels: the pool's j-th prime, the guard last */
static uint64_t lc_prime(uint32_t j, uint32_t m) { return j + 1 == m ? (uint64_t)lc_guard_ : (uint64_t)lc_pool_[j]; }
static uint64_t lc_inv(uint64_t a, uint64_t p) {        /* a^-1 mod p, p prime */
  int64_t t = 0, nt = 1, r = (int64_t)p, nr = (int64_t)(a % p);
  while (nr) { int64_t q = r / nr, x; x = t - q * nt; t = nt; nt = x; x = r - q * nr; r = nr; nr = x; }
  return (uint64_t)(t < 0 ? t + (int64_t)p : t);
}

/* The piece a cell holds, into `out` (at least 16 bytes): its length, or -1 when the row is not a piece — a
   sign, a denominator other than 1, residues that are not one number, or no 1 above the top. */
static int lc_cell_piece(const uint8_t *row, uint64_t n, uint8_t *out) {
  lc_init();
  if (!row || n < 9 || (n - 1) % 8) return -1;
  const uint32_t m = (uint32_t)((n - 1) / 8), k = m - 1;
  if (k < 1 || k > 4 || row[0]) return -1;
  uint32_t r[5], d;
  for (uint32_t j = 0; j < m; j++) {
    memcpy(&r[j], row + 1 + 4 * (uint64_t)j, 4);
    memcpy(&d, row + 1 + 4 * ((uint64_t)m + j), 4);
    if (d != 1) return -1;
  }
  unsigned __int128 x = r[0] % lc_prime(0, m), M = lc_prime(0, m);
  for (uint32_t i = 1; i < k; i++) {                    /* Garner: one data prime at a time */
    const uint64_t p = lc_prime(i, m), xm = (uint64_t)(x % p), mm = (uint64_t)(M % p);
    const uint64_t t = (uint64_t)((unsigned __int128)((r[i] % p + p - xm) % p) * lc_inv(mm, p) % p);
    x += M * t; M *= p;
  }
  if ((uint64_t)(x % (uint64_t)lc_guard_) != r[k] % (uint64_t)lc_guard_) return -1;   /* the guard says one number */
  int len = 0;
  while (x) { out[len++] = (uint8_t)x; x >>= 8; }
  if (!len || out[len - 1] != 1) return -1;
  return len - 1;
}
/* Make a row of `n` bytes hold this piece (len <= LC_PIECE): a cell the engine would read as these bytes. */
static int lc_cell_make(const uint8_t *piece, int len, uint8_t *row, uint64_t n) {
  lc_init();
  if (!row || n < 9 || (n - 1) % 8 || len < 0 || len > LC_PIECE) return -1;
  const uint32_t m = (uint32_t)((n - 1) / 8), one = 1;
  unsigned __int128 x = 1;                              /* the 1 above the top, then the piece below it */
  for (int i = len; i-- > 0;) x = (x << 8) | piece[i];
  row[0] = 0;
  for (uint32_t j = 0; j < m; j++) {
    const uint32_t v = (uint32_t)(x % lc_prime(j, m));
    memcpy(row + 1 + 4 * (uint64_t)j, &v, 4);
    memcpy(row + 1 + 4 * ((uint64_t)m + j), &one, 4);
  }
  return 0;
}

#endif /* TESTS_LEAF_CELLS_H */
