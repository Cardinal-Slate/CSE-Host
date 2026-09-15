/* Deterministic seeded PRNG for the test harness: splitmix64 seeding into xoshiro256**.
 *
 * Header-only, freestanding, integer-only (no floats, no allocation, no libc). The contract is
 * reproducibility: one seed produces one stream, bit-identical on every target
 * (arm64/x86_64/wasm) in every release — a fuzz failure that names its (count, seed) pair is a
 * complete reproduction. unit/prng.c freezes the stream with known-answer vectors.
 *
 * Not cryptographic: xoshiro256** is a statistical generator (period 2^256-1, passes BigCrush).
 * Algorithm: Blackman & Vigna, "Scrambled linear pseudorandom number generators" (2018);
 * implemented here from the published recurrence.
 */
#ifndef SLATE_PRNG_H
#define SLATE_PRNG_H

#include <stdint.h>

typedef struct {
  uint64_t s[4];
} slate_prng;

static inline uint64_t slate_prng_rotl_(uint64_t x, int k) {
  return (x << k) | (x >> (64 - k));
}

/* splitmix64 expands the one-word seed into the four-word state: distinct seeds give
 * decorrelated states, and seed 0 is as good as any other. */
static inline void slate_prng_seed(slate_prng *g, uint64_t seed) {
  uint64_t z = seed;
  for (int i = 0; i < 4; i++) {
    z += 0x9E3779B97F4A7C15ull;
    uint64_t t = z;
    t = (t ^ (t >> 30)) * 0xBF58476D1CE4E5B9ull;
    t = (t ^ (t >> 27)) * 0x94D049BB133111EBull;
    g->s[i] = t ^ (t >> 31);
  }
  if (!(g->s[0] | g->s[1] | g->s[2] | g->s[3])) g->s[0] = 1; /* the all-zero state is fixed */
}

/* next 64 uniform bits */
static inline uint64_t slate_prng_next(slate_prng *g) {
  uint64_t *s = g->s;
  uint64_t out = slate_prng_rotl_(s[1] * 5, 7) * 9;
  uint64_t t = s[1] << 17;
  s[2] ^= s[0];
  s[3] ^= s[1];
  s[1] ^= s[2];
  s[0] ^= s[3];
  s[2] ^= t;
  s[3] = slate_prng_rotl_(s[3], 45);
  return out;
}

/* uniform in [0, n) — unbiased via rejection of the 2^64 mod n overhang (n=0/1 return 0) */
static inline uint64_t slate_prng_below(slate_prng *g, uint64_t n) {
  if (n < 2) return 0;
  uint64_t lim = (0 - n) % n; /* = 2^64 mod n */
  uint64_t r;
  do {
    r = slate_prng_next(g);
  } while (r < lim);
  return r % n;
}

/* uniform in [lo, hi] inclusive (lo > hi returns lo) */
static inline uint64_t slate_prng_range(slate_prng *g, uint64_t lo, uint64_t hi) {
  if (lo >= hi) return lo;
  return lo + slate_prng_below(g, hi - lo + 1);
}

#endif /* SLATE_PRNG_H */
