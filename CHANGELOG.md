# Changelog

Semantic versions (`MAJOR.MINOR.PATCH`). Pre-1.0, a MINOR release may change API; a PATCH release is fixes only.

## 0.4.0

- **`slate_dag_program` and `slate_dag_start`** (embed band). The first sets the word of the row a container
  runs; the second runs it through the trampoline, refusing with no program, a cleared one, or a word the store
  does not hold. An entrypoint is a store, a secret and a word; it builds no graph. The trampoline is one loop
  with two entries (`run_ticks`): a run that handed off, and a start.

## 0.3.0

- **`slate_dag_lens`** (embed band): pin the builder's prime set. A pinned region asks for the row this lens
  names and refuses a prime that would make a bad modulus rather than substituting one; two disjoint lenses over
  one construction are two rows, the same lens twice is one. It sits in the embedder band because it changes
  which row is asked for, not what it costs.
- **The run trampoline resolves by word.** The tail slot holds the next program's word; each tick reads the
  program from the store before loading it, and a word the store does not hold ends the dispatch as a refusal.
  `tests/unit/abi_embed.c` gates both: the lens (three rows, one row, refused args) and the program as a row
  (emit yields a word, run and tail take it in this container or another, an unkept word refuses).

## 0.2.2

- `tests/unit/abi_embed.c` states what holds with memory, not what holds without it.

## 0.2.1

- `tests/unit/abi_embed.c`: the blocks whose subject is the row install a store; the rest do not. A check that
  the host was reached is a claim about work, and a granted ask whose row is there is served from it.
## 0.2.0

- **The C ABI is two bands.** `include/slate/slate.h` (the user band: a record from binaries, run, the receipt and
  records, the store via `slate_dag_codec`, stop/restore, fragments, `slate_invoke`), `include/slate/embed.h` (an
  embedder's effects, sandbox, consume cursor, fibers, search) and `include/slate/tune.h` (the cost knobs) replace
  `array.h`; `slate_refused` lives in `slate.h`. Removed: `number.h` (`slate_num_*`), `reals.h` (`slate_real_read`),
  the batch door's implementation (`slate_map*`, `slate_reduce`, `slate_prog_admits`) and the `slate_array_i64` alias.
  Gates: `tests/unit/abi_slate.c`, `tests/unit/abi_embed.c` (pure C); `tests/unit/engine/cabi.c` and
  `tests/unit/number_cabi.c` removed.
- The C door decodes `min` / `max` program words; `slate_num_ctx_new_sized`'s cap no longer forces a read.
One evaluator, every reading on the arc: every C door decodes program words into the record and reads through Engine::map; fragment v5. Synced from CSE-Core 0.2.0.

## 0.1.2

README: the includes that cross the dependency stack, listed.

## 0.1.1

Remove `tests/support/prng.h`, a test helper only CSE-RNS's gates use; it was copied into every part and collided in the umbrella's merged view.

## 0.1.0

Extracted from CSE-Core `6d3be1b`: The C ABI and the composition root. Paths unchanged; gates carried with the code.
