/* A construction, its selection and its answers cross the existing store door. New builders on every call;
 * no installed flag or answer cache in the caller. SPDX-License-Identifier: MIT OR Apache-2.0 */
#include <stdio.h>
#include "slate/transfer.h"
#include "../../memstore.h"

static int fails;
static const uint8_t scope[] = "transfer-test";
static const uint8_t name[] = "rns.mul31";
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL transfer: %s (line %d)\n", m, __LINE__); fails++; } } while (0)

static SlateDag *builder(MemStore *s) {
  SlateDag *b = slate_dag_new();
  if (!b) { fprintf(stderr, "builder allocation failed\n"); exit(1); }
  MS_INSTALL(b, s);
  return b;
}
static uint8_t *program_file(const char *path, uint64_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f || fseek(f, 0, SEEK_END)) { fprintf(stderr, "cannot read %s\n", path); exit(1); }
  long len = ftell(f);
  if (len <= 0 || fseek(f, 0, SEEK_SET)) exit(1);
  uint8_t *bytes = (uint8_t *)malloc((size_t)len);
  if (!bytes || fread(bytes, 1, (size_t)len, f) != (size_t)len) exit(1);
  fclose(f); *n = (uint64_t)len; return bytes;
}
static void compare(MemStore *s, uint64_t a, uint64_t c, uint64_t p) {
  SlateDag *b = builder(s); uint64_t out = UINT64_MAX; int stored = 0;
  const char *rc = slate_transfer_mul31(b, scope, sizeof scope - 1, a, c, p, 0, &out, &stored);
  CHECK(rc == NULL, "stored run succeeds with legacy path disabled");
  CHECK(stored == 1 && out == (a*c)%p, "stored run equals the original expression");
  slate_dag_free(b);
}
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *path = argc > 1 ? argv[1] : "core/programs/rns/mul31.slate";
  uint64_t pn = 0; uint8_t *program = program_file(path, &pn);
  MemStore s; ms_init(&s, 1 << 16);
  SlateDag *b = builder(&s); uint64_t out = 999; int stored = -1;
  const char *rc = slate_transfer_mul31(b, scope, sizeof scope - 1, 6, 7, 31, 0, &out, &stored);
  CHECK(rc && !strcmp(rc, "missing") && out == 999, "uninstalled and no legacy: missing, no answer");
  CHECK(!slate_transfer_mul31(b, scope, sizeof scope - 1, 6, 7, 31, 1, &out, &stored), "uninstalled legacy path");
  CHECK(out == 11 && stored == 0, "legacy result is identified");
  CHECK(!slate_transfer_write(b, scope, sizeof scope - 1, name, sizeof name - 1, program, pn), "write and select program");
  slate_dag_free(b);
  puts("PASS: absent selection, explicit legacy path, write and select");

  const uint64_t moduli[] = {2, 3, 7, 65521, 1073741827, 2147483647};
  for (unsigned j = 0; j < sizeof moduli / sizeof moduli[0]; j++) {
    uint64_t p = moduli[j];
    compare(&s, 0, 0, p); compare(&s, 0, p-1, p); compare(&s, 1, p-1, p); compare(&s, p-1, p-1, p);
  }
  uint64_t rng = 0x726e732d73746f72ULL;
  for (unsigned i = 0; i < 64; i++) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    uint64_t a = rng % 2147483647ULL;
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    compare(&s, a, rng % 2147483647ULL, 2147483647);
  }
  puts("PASS: 88 boundary/random comparisons with legacy disabled, fresh builders");
  compare(&s, 6, 7, 31);
  long puts_before = s.puts; size_t rows_before = s.rows;
  compare(&s, 6, 7, 31);
  CHECK(s.puts == puts_before && s.rows == rows_before, "same input reuses kept answer without writes");

  b = builder(&s); uint8_t *selected = NULL; uint64_t wn = 0;
  CHECK(!slate_transfer_read(b, scope, sizeof scope - 1, name, sizeof name - 1, &selected, &wn), "read selection back");
  CHECK(wn != 0, "selection is a program word");
  uint8_t *readback = NULL; uint64_t rn = 0;
  CHECK(!slate_dag_leaf_read(b, selected, wn, &readback, &rn) && rn == pn && !memcmp(readback, program, (size_t)pn), "stored construction matches artifact");
  free(readback);
  const uint8_t other[] = "other-scope";
  uint8_t *w = NULL; uint64_t n = 0;
  rc = slate_transfer_read(b, other, sizeof other - 1, name, sizeof name - 1, &w, &n);
  CHECK(rc && !strcmp(rc, "missing"), "scope isolates the selection"); free(w);
  const uint8_t bad[] = {1, 2, 3};
  CHECK(slate_transfer_write(b, scope, sizeof scope - 1, name, sizeof name - 1, bad, sizeof bad) != NULL, "malformed construction cannot replace selection");
  CHECK(!slate_transfer_mul31(b, scope, sizeof scope - 1, 6, 7, 31, 0, &out, &stored) && out == 11 && stored, "previous selection survives failed install");
  CHECK(slate_transfer_mul31(b, scope, sizeof scope - 1, 31, 1, 31, 1, &out, &stored) != NULL, "out-of-domain input refuses");
  slate_dag_free(b);
  puts("PASS: readback, answer reuse, scope isolation, failed install preserves selection");

  /* A new valid construction under the same name must select a new identity, not return the old answer. */
  b = builder(&s);
  uint8_t placeholder[12] = {0}; uint32_t hole = slate_dag_carrier_bytes(b, placeholder, 12);
  int32_t root = slate_dag_load(b, hole, slate_dag_lit(b, 0));
  uint8_t *alternate = NULL; uint64_t an = 0;
  CHECK(!slate_dag_save_fragment(b, root, &hole, 1, &alternate, &an), "save alternate construction");
  uint8_t *alternate_bytes = NULL; uint64_t abn = 0;
  CHECK(!slate_dag_leaf_read(b, alternate, an, &alternate_bytes, &abn), "read alternate construction bytes");
  CHECK(!slate_transfer_write(b, scope, sizeof scope - 1, name, sizeof name - 1, alternate_bytes, abn), "select alternate");
  slate_dag_free(b); free(alternate); free(alternate_bytes);
  b = builder(&s);
  CHECK(!slate_transfer_mul31(b, scope, sizeof scope - 1, 6, 7, 31, 0, &out, &stored) && out == 6 && stored, "selection change does not reuse previous program's answer");
  /* A dangling selection must fail even when the caller allows legacy. */
  CHECK(!slate_dag_named_put(b, scope, sizeof scope - 1, name, sizeof name - 1, bad, sizeof bad), "set dangling selection");
  out = 999;
  rc = slate_transfer_mul31(b, scope, sizeof scope - 1, 6, 7, 31, 1, &out, &stored);
  CHECK(rc && strcmp(rc, "missing") && out == 999 && !stored, "selected missing program refuses, no legacy fallback");
  CHECK(!slate_dag_named_put(b, scope, sizeof scope - 1, name, sizeof name - 1, NULL, 0), "set empty selection");
  rc = slate_transfer_mul31(b, scope, sizeof scope - 1, 6, 7, 31, 1, &out, &stored);
  CHECK(rc && !strcmp(rc, "program") && out == 999 && !stored, "empty selection is broken, not absent");
  slate_dag_free(b);
  b = builder(&s);
  CHECK(!slate_transfer_write(b, scope, sizeof scope - 1, name, sizeof name - 1, program, pn), "restore original selection");
  slate_dag_free(b); compare(&s, 29, 30, 31);
  free(selected); free(program); ms_free(&s);
  /* The named door distinguishes an invalid value prefix from an absent name. */
  MemStore broken; ms_init(&broken, 64); b = builder(&broken);
  CHECK(!slate_dag_named_put(b, scope, sizeof scope - 1, name, sizeof name - 1, bad, sizeof bad), "keep isolated entry");
  const uint8_t invalid_prefix[] = {0, 1, 2, 3};
  CHECK(ms_leaf_write(&broken, invalid_prefix, sizeof invalid_prefix), "damage named-value prefix");
  out = 999;
  rc = slate_transfer_mul31(b, scope, sizeof scope - 1, 6, 7, 31, 1, &out, &stored);
  CHECK(rc && !strcmp(rc, "entry") && out == 999 && !stored, "corrupt entry refuses instead of taking legacy path");
  slate_dag_free(b); ms_free(&broken);
  puts("PASS: changed selection, dangling selection refusal, restored selection");
  printf("%s transfer_cabi (%d failures)\n", fails ? "FAIL" : "PASS", fails);
  return fails ? 1 : 0;
}
