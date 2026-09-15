/* The batch door from pure C. No C++ in this translation unit: includes slate/batch.h only, builds a
 * fold-dialect program by hand ((s+2)/(s+3) — coprime, so records are predictable), sizes the out buffer
 * with slate_map_stride, runs the batch, verifies every record, and checks the engine accounts nonzero
 * reserved bytes. Runs with or without a device backend linked (the routing is the Engine's business; the
 * values are the same either way). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/batch.h"

#define OP(op, x) (((uint32_t)(op) << 16) | ((uint32_t)(x) & 0xffff))

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int fails = 0;
  /* v = (s + 2) / (s + 3) */
  const uint32_t prog[] = {OP(11, 0), OP(0, 2), OP(1, 0), OP(11, 0), OP(0, 3), OP(1, 0), OP(9, 0)};
  const uint64_t plen = sizeof prog / sizeof prog[0];
  const uint32_t m = 512;

  uint64_t stride = slate_map_stride(prog, plen, m, 0);
  if (stride == 0) { printf("FAIL test_engine_cabi: stride refused a bounded program\n"); return 1; }

  uint64_t *out = (uint64_t *)malloc((size_t)m * stride * 8);
  uint64_t got_stride = 0;
  int rc = slate_map(prog, plen, m, 64, 0, 0, out, (uint64_t)m * stride * 8, &got_stride);
  if (rc != 0 || got_stride != stride) {
    printf("FAIL test_engine_cabi: map rc=%d stride=%llu (want %llu)\n", rc,
           (unsigned long long)got_stride, (unsigned long long)stride);
    return 1;
  }
  uint64_t L = (stride - 3) / 2;
  for (uint32_t s = 0; s < m; s++) {
    const uint64_t *r = out + (size_t)s * stride;
    /* gcd(s+2, s+3) = 1: the record is exactly (s+2)/(s+3), higher limbs zero */
    int ok = r[0] == 1 && r[1] == 0 && r[2] == L && r[3] == s + 2 && r[3 + L] == s + 3;
    for (uint64_t i = 1; ok && i < L; i++) ok = r[3 + i] == 0 && r[3 + L + i] == 0;
    if (!ok) {
      printf("FAIL test_engine_cabi: shard %u record wrong (valid=%llu sign=%llu num0=%llu den0=%llu)\n",
             s, (unsigned long long)r[0], (unsigned long long)r[1],
             (unsigned long long)r[3], (unsigned long long)r[3 + L]);
      fails++;
      break;
    }
  }
  free(out);

  /* refusals are clean returns with the distinct locked code, not aborts */
  uint64_t tiny[8];
  int rc2 = slate_map(prog, plen, m, 64, 0, 0, tiny, sizeof tiny, NULL);
  if (rc2 != SLATE_BATCH_EOUTSIZE) {
    printf("FAIL test_engine_cabi: undersized out rc=%d (want EOUTSIZE=%d)\n", rc2,
           SLATE_BATCH_EOUTSIZE); fails++;
  }
  rc2 = slate_map(prog, plen, 0, 64, 0, 0, tiny, sizeof tiny, NULL);
  if (rc2 != SLATE_BATCH_EARGS) {
    printf("FAIL test_engine_cabi: empty batch rc=%d (want EARGS=%d)\n", rc2,
           SLATE_BATCH_EARGS); fails++;
  }
  {
    /* control flow is not statically boundable: without a hint the call must say so */
    const uint32_t loopy[] = {OP(0, 1), OP(4, 0)};
    rc2 = slate_map(loopy, 2, m, 64, 0, 0, tiny, sizeof tiny, NULL);
    if (rc2 != SLATE_BATCH_EUNBOUNDED) {
      printf("FAIL test_engine_cabi: unbounded program rc=%d (want EUNBOUNDED=%d)\n", rc2,
             SLATE_BATCH_EUNBOUNDED); fails++;
    }
  }

  /* a lying hint refuses the call (nonzero), never aborts and never truncates a record:
   * cidx^32 is ~256 bits at shard 255, but nb=db=1 declares K=4, L=3 limbs (192 bits) —
   * the wide shards exceed the declared bound and the whole call must refuse. */
  {
    uint32_t lie[64];
    uint64_t lplen = 0;
    lie[lplen++] = OP(11, 0);                       /* cidx */
    for (int i = 0; i < 31; i++) {
      lie[lplen++] = OP(11, 0);                     /* cidx */
      lie[lplen++] = OP(2, 0);                      /* mul */
    }
    uint64_t lhint = 1ull | (1ull << 32);           /* nb=1, db=1: a lie for cidx^32 */
    static uint64_t lout[512 * 16];
    int lrc = slate_map(lie, lplen, 256, 64, 0, lhint, lout, sizeof lout, NULL);
    if (lrc != SLATE_BATCH_EREFUSED) {
      printf("FAIL test_engine_cabi: lying hint rc=%d (want EREFUSED=%d)\n", lrc,
             SLATE_BATCH_EREFUSED); fails++;
    }
  }

  if (slate_map_bytes() == 0) {
    printf("FAIL test_engine_cabi: engine accounts zero bytes after a batch\n"); fails++;
  }

  printf("%s test_engine_cabi: slate_map from pure C (%u shards, stride %llu, bytes %zu)\n",
         fails ? "FAIL" : "PASS", m, (unsigned long long)stride, slate_map_bytes());
  return fails ? 1 : 0;
}
