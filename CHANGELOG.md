# Changelog

Semantic versions (`MAJOR.MINOR.PATCH`). Pre-1.0, a MINOR release may change API; a PATCH release is fixes only.

## 0.7.0

- **`slate_dag_roster`** (embed band): the whole lens a container's words name, and its division. The primes
  take the same rule as `slate_dag_lens` (at least 2, under 2^24, prime, not the pool's guard, no repeats);
  `k` 0 clears it. With a roster set, `slate_dag_lens` must name exactly one share of it —
  `roster[j*k/n, (j+1)*k/n)` for some unit j, n = min(shares, k), shares 0 or 1 = one unit — else `"args"`,
  and so does a `slate_dag_shares` whose split would leave the pinned primes astride it. That is the one split
  rule; the door in front of the store runs the same arithmetic, so the two name the same share. A container
  never carries channels that are not a share of the lens its words name, so `slate_dag_lens(b, NULL, 0)` with
  a roster set refuses too.
- **`slate_dag_shares`** keeps its signature and means shares over the roster. A run never spreads in process:
  the container computes its own share and nothing else, and what carries the other shares is the placement
  door's question, never the engine's.
- **The ask: `slate_dag_ask` / `slate_dag_take_ask`.** The bytes that let another machine run the same
  construction on its own share — the program's word, the last dispatch's dims, the roster and the shares, and
  nothing else. Host's own format, little-endian, byte exact, versioned by its leading tag (embed.h spells it):
  `[0x41 'A'][version u8 = 1][shares u32][k u32][roster prime i64]*k [ndims u32][dim i64]*ndims [wn u64][word u8]*wn`.
  A reader refuses a tag or version it does not know, and every field is bounds-checked against the ask's end.
  `slate_dag_take_ask` configures a container from one with `primes` as its own share; `slate_dag_start` with
  no dims named runs over the dims the ask brought.
- **A construction built through the API is named before it is run.** At `slate_dag_run` with a roster set and
  no program word, the graph is kept as a program row first — its fragment bytes under their own word, the same
  keep the emit door makes — and that word becomes the container's program, so the ask names the construction
  rather than carrying it. A graph that is not fragment-addressable (an RNS/ℚ, f32 or wide carrier) is not
  kept; the run is unaffected and the ask names no program.
- **`slate_array_cell_word`**: the word of cell i of a reading — the reading's own word with the index after
  it, the same name the engine keeps that cell's row under. A deployment asks the door in front of the store
  whether the root is whole with it; the engine is asked nothing.
- The orchestrate seam is gone from the tree, and with it its mention in `slate.h`'s provider header list.

## 0.6.0

- `slate_dag_lens` refuses a pinned prime that is not prime, not distinct, outside the pool's range or the
  guard, with the engine's own `is_prime` — the C door's rule is the pool's rule (refusal `"args"`).
- One body each for the two dispatch doors, the two carrier-swap doors and the int64 readback; the pasted dead
  branch and the unwired `FragOps::run` slot are gone. The CRC table is a compile-time constant. The declared
  height lives on the Envelope's `height_bits` only; `carrier_q` raises it there. Exported C symbols unchanged.

## 0.5.0

- **`slate_dag_shares`.** The primes set by `slate_dag_lens` divide into shares a run spreads over. The tail
  trampoline reads a program's row through the one reader.

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
