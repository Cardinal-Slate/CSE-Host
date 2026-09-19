# Changelog

Semantic versions (`MAJOR.MINOR.PATCH`). Pre-1.0, a MINOR release may change API; a PATCH release is fixes only.

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
