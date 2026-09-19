/* The io door names an open resource by the word of the effect that opened it — never by its fd. From pure C over
 * the array ABI: mount a tmpfs, open a file for writing, write, close, open it for reading, read it back through the
 * words the opens returned; two builders opening the same path get the same word (content, not a handle); a read
 * presenting a word no open produced refuses. No C++ in this translation unit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slate/slate.h"
#include "slate/embed.h"

/* the io verbs, by name: a blob is the verb, then (after a NUL) its own bytes — open's mode words, read's u32 chunk */
static const uint8_t OPEN_W[] = "open\0write";
static const uint8_t OPEN_R[] = "open";
static const uint8_t WRITE[]  = "write";
static const uint8_t CLOSE[]  = "close";
static const uint8_t READ16[] = { 'r', 'e', 'a', 'd', 0, 16, 0, 0, 0 };   /* read a chunk of 16 */
static const char *const IO = "io";
static const char *const CAPS[] = { "io" };

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL io_word_cabi: %s\n", msg); fails++; } } while (0)

/* read `n` cells of carrier `cid` after a run: root = load(cid, param 0) over dims {n}; -1 if the run refused. */
static int read_cells(SlateDag *b, uint32_t cid, int64_t n, int64_t *out) {
  int32_t root = slate_dag_load(b, cid, slate_dag_param(b, 0));
  int64_t dims[1] = { n };
  SlateArray *a = slate_dag_run(b, root, dims, 1);
  if (!a) return -1;
  const slate_entry *re; int32_t rn;
  int refused = (slate_array_receipt(a, &re, &rn) == NULL && slate_entry_is(re, rn, "verdict", "undefined"));
  int64_t *den = (int64_t *)malloc((size_t)n * 8);
  const char *rc = slate_array_i64_unsafe(a, out, den);
  free(den);
  slate_array_free(a);
  return (refused || rc != NULL) ? -1 : 0;
}

/* the chain: open(write) → write "hello" → close → open(read) → read. Returns the open-for-write carrier and the
 * read result carrier. */
static void chain(SlateDag *b, uint32_t *open_w, uint32_t *read_out) {
  slate_dag_effect_caps(b, CAPS, 1);
  slate_dag_tmpfs(b, "/t", 1);
  uint32_t addr = slate_dag_carrier_bytes(b, (const uint8_t *)"file:/t/x", 9);
  *open_w = slate_dag_effect_io(b, IO, OPEN_W, sizeof OPEN_W, &addr, 1, 64);   /* open for writing */
  uint32_t data = slate_dag_carrier_bytes(b, (const uint8_t *)"hello", 5);
  uint32_t wops[2] = { *open_w, data };
  (void)slate_dag_effect_io(b, IO, WRITE, 5, wops, 2, 8);
  (void)slate_dag_effect_io(b, IO, CLOSE, 5, open_w, 1, 8);
  uint32_t open_r = slate_dag_effect_io(b, IO, OPEN_R, 4, &addr, 1, 64);   /* open for reading */
  *read_out = slate_dag_effect_io(b, IO, READ16, sizeof READ16, &open_r, 1, 16);
}

int main(void) {
  /* 1. the bytes come back through the words: the resource is reached by name, never by fd */
  int64_t w1[8] = {0};
  {
    SlateDag *b = slate_dag_new();
    uint32_t open_w, read_out;
    chain(b, &open_w, &read_out);
    int64_t got[5] = {0};
    CHECK(read_cells(b, read_out, 5, got) == 0, "read through the open's word ran");
    CHECK(got[0] == 'h' && got[1] == 'e' && got[2] == 'l' && got[3] == 'l' && got[4] == 'o', "read back the bytes written");
    CHECK(read_cells(b, open_w, 8, w1) == 0, "the open's out carrier is readable");
    /* the open returned a word (8 bytes under the default encoder), not the tmpfs handle number (2^40 as 8 LE bytes) */
    int is_fd = (w1[0] == 0 && w1[1] == 0 && w1[2] == 0 && w1[3] == 0 && w1[4] == 0 && w1[5] == 1 && w1[6] == 0 && w1[7] == 0);
    int nonzero = 0; for (int i = 0; i < 8; i++) if (w1[i]) nonzero = 1;
    CHECK(!is_fd && nonzero, "the open returned the effect's word, not the fd");
    slate_dag_free(b);
  }
  /* 2. the same open in another builder, same encoder: the same word (the name is content, not a handle) */
  {
    SlateDag *b = slate_dag_new();
    uint32_t open_w, read_out;
    chain(b, &open_w, &read_out);
    int64_t w2[8] = {0};
    CHECK(read_cells(b, open_w, 8, w2) == 0, "second builder's open readable");
    CHECK(memcmp(w1, w2, sizeof w1) == 0, "two builders name the same open by the same word");
    slate_dag_free(b);
  }
  /* 3. a word no open produced refuses: the resource cannot be reached by guessing */
  {
    SlateDag *b = slate_dag_new();
    slate_dag_effect_caps(b, CAPS, 1);
    slate_dag_tmpfs(b, "/t", 1);
    uint8_t fake[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint32_t bogus = slate_dag_carrier_bytes(b, fake, 8);
    uint32_t out = slate_dag_effect_io(b, IO, READ16, sizeof READ16, &bogus, 1, 16);
    int64_t got[1] = {0};
    CHECK(read_cells(b, out, 1, got) == -1, "a read on an unknown word refuses");
    slate_dag_free(b);
  }
  if (fails == 0) printf("PASS io_word_cabi: an open resource is named by its effect's word; read by name; unknown word refuses\n");
  return fails ? 1 : 0;
}
