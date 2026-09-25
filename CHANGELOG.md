# Changelog

Semantic versions (`MAJOR.MINOR.PATCH`). Pre-1.0, a MINOR release may change API; a PATCH release is fixes only.

## 0.12.0

`slate_dag_leaf(b, bytes, n, &word, &wn)`: bytes handed in are kept as a leaf through the container's own door —
the same `leaf_key` / `leaf_put` the ask and a fragment go through, one cell per byte on the roster, sliced across
the carriers — and the leaf's word comes back. It is how a unit keeps bytes a client hands it, so no byte is
ever kept raw. A compatible addition to the C ABI.

The host effect's read, write and accept no longer wait on a timer. They asked the cancel gate every 200 ms
around a `poll`; the gate is asked once, at the effect boundary, and the verb runs and the wire holds it. The
embedder gate's fiber block no longer sleeps to make its four hosts answer late: each answers once all four have
begun, which is all four parked at once.

The declared work bounds a program's ticks. `slate_dag_work` was a real read's refine-step cap; it is now the
container's work, and a program that tails into the next — or into itself — spends one step of it per tick. A run
that spends it stops with its last tick's reading, Incomplete with obligation Resume; a run with a larger bound
replays every kept tick as a hit. Nothing reads a clock to stop a run.

## 0.11.0

- **The division leaves the container, and the word.** How many units a roster is cut into was a setting here
  and a field in every ask; it is neither now. It is the deployment's rule — the same on every unit, read at
  start and asked of the placement seam — so re-dividing a roster changes no word it ever named and forces no
  recompute. What the engine says about a share is the one thing that survives any division: it is a
  contiguous run of the roster.
- **Breaking, `slate_dag_roster` loses its `shares`:**
  `const char *slate_dag_roster(SlateDag *b, const int64_t *primes, uint32_t k)` — was
  `(SlateDag *, const int64_t *primes, uint32_t k, uint32_t shares)`. It sets the roster and nothing else.
  A caller that passed a share count drops the argument; nothing replaces it in this band.
- **Breaking, `slate_dag_shares` is removed.** There is no door for a division on a container. Callers that set
  it beside `slate_dag_lens` delete the call.
- **Breaking, `slate_dag_lens` takes any contiguous sub-range of the roster.** With a roster set, `primes` must
  be `roster[lo, hi)` for some `0 <= lo < hi <= k` — which is what every division of a roster hands a unit —
  else `"args"`. Wider than before (a run that one particular division would cut is now a share, and so is the
  whole roster: the undivided deployment) and no looser where it counts: a set with a gap, one out of the
  roster's order, one with a prime from outside it, and `k` 0 with a roster set are all still `"args"`.
- **Breaking, the ask's byte format loses its first field:**
  `[k u32][roster prime i64]*k [ndims u32][dim i64]*ndims [wn u64][program word u8]*wn` — was
  `[shares u32][k u32][roster prime i64]*k …`. Still little-endian, fixed order, byte exact, no tag and no
  version; the roster's count is now the very first thing in it. An ask written by 0.10.0 does not read here.
- `slate_dag_take_ask` and `slate_dag_run_ask` are otherwise unchanged, signatures included. `run_ask`'s
  stopped state ("this unit carries one share of a roster someone else carries") is read off the primes — a
  lens that is a proper run of the roster — rather than off a share count.
- Tests (`tests/unit/abi_embed.c`, block 9): the ask's first field is the roster's count and the roster follows
  it; a lens with a gap, out of order, or outside the roster refuses, the whole roster and an off-cut run are
  taken; a roster the pinned primes do not run in refuses; no `shares` door anywhere.

## 0.10.0

- **The ask is a leaf, run by its word.** Two packets and no third: the cells under a word, and a row under a
  word. The ask's bytes were the last thing that travelled beside them, and they no longer do — they go
  through the door like every other bytes handed in, one cell per byte on the roster lens
  (`Arena::leaf_key` ‖ `Slate::leaf_put`, exactly the keep the construction gets), and what comes back is the
  ask's word. Nothing is kept under the bare word: a leaf lives in its cells.
- **Breaking, the two ask doors take and give a word:**
  - `int slate_dag_ask(SlateDag *b, uint8_t **word, uint64_t *wn)` — was `(const SlateDag *, uint8_t **out, uint64_t *n)`
    handing back the bytes. It keeps the ask and hands back its word; nonzero when the store kept nothing.
    (The container is no longer `const`: keeping is what the call does.)
  - `const char *slate_dag_take_ask(SlateDag *b, const uint8_t *word, uint64_t wn, const int64_t *primes, uint32_t k)`
    — was `(SlateDag *, const uint8_t *ask, uint64_t n, ...)`. It walks the ask's leaf out of this container's
    store (`word ‖ 0`, `word ‖ 1`, … to the first cell nobody has — the records are the count), then configures
    the container as before. A word nothing is kept under is `"args"`; a leaf the door could only half gather
    is `"partial"` — not whole is never the end of a leaf, and never a short ask.
  - The ask's byte format is unchanged: `[shares u32][k u32][roster prime i64]*k [ndims u32][dim i64]*ndims [wn u64][program word u8]*wn`,
    no tag and no version. What changed is that a reader gets those bytes from the store, never from a caller.
- **New, `slate_dag_run_ask`**: a unit's whole answer to an Interest for an ask's word — an Interest for such a
  word is "run it".
  `const char *slate_dag_run_ask(SlateDag *b, const uint8_t *word, uint64_t wn, const int64_t *primes, uint32_t k, uint8_t **root, uint64_t *rn)`:
  take the ask, start the program it names over the dims it carries, and hand back the root reading's own word.
  NULL when the root is whole, `"share"` when this unit's share landed and the root is not whole (the stopped
  state), `"args"` / `"partial"` / `"refused"` otherwise. Whole is read off the store and never remembered:
  cell 0's row through the door in front of this container's store — the row is there (every share of that cell
  landed) and the root is whole; not-whole says another carrier has not landed its share.
- **New, `slate_array_word`**: `const char *slate_array_word(const SlateArray *a, uint8_t **out, uint64_t *n)`
  — the reading's own word, the key its per-cell rows are kept under, so a reader walks the root cell by cell.
  `slate_array_cell_word` is unchanged and stays: it is that key with a cell's index after it.
- `tests/unit/abi_embed.c` block 9 states it: the ask is one byte cell per byte with nothing under the bare
  word, its fields read back off those cells, the same context names the same ask and is never kept twice, a
  word nothing is kept under is `"args"`, a half-gathered leaf is `"partial"`, a store that lies about the
  ask's own cells is refused where it is read, and `slate_dag_run_ask` by that word hands back the root's word
  — the same word twice over one store.

## 0.9.0

- **There is no count anywhere.** Not beside a word, not in a name, not in the ask, not as a row. A leaf is
  read by walking it — `word ‖ 0`, `word ‖ 1`, … to the first cell nobody has — so the records are the count.
  Every program reader here walks.
- **A program's name is its word.** `slate_dag_program(SlateDag *b, const uint8_t *word, uint64_t n)` takes
  the bare word (the signature is unchanged; what it means is). `Envelope::program`, the tail slot,
  `"slate.emit"`'s answer, `"slate.run"`'s request and io operand and `"slate.tail"`'s operand all carry that
  one thing.
- **Breaking, the three fragment doors lose their count:**
  - `const char *slate_dag_save_fragment(SlateDag *b, int32_t root, const uint32_t *holes, uint32_t nholes, uint8_t **word, uint64_t *wn)`
  - `SlateFrag *slate_frag_load(SlateDag *b, const uint8_t *word, uint64_t wn)`
  - `SlateArray *slate_invoke(SlateDag *b, const uint8_t *word, uint64_t wn, const uint8_t *const *args, const uint64_t *arg_lens, uint32_t nargs, const char *const *caps, uint32_t ncaps, const int64_t *dims, uint32_t ndims)`
- **Breaking, the ask loses `pn`:**
  `[shares u32][k u32][roster prime i64]*k [ndims u32][dim i64]*ndims [wn u64][program word u8]*wn`.
  `wn` 0 is no program. Nothing rides beside the word — the taker walks the leaf out of its own store.
- **A walk that stops early is the parser's to catch.** Nothing outside the bytes says how long a program is,
  so a store that lost a cell, or a share that has not landed, hands the parser a short program — and the crc
  over the body plus the exact-end check refuse it every time. A leaf the door in front of the store could
  only half gather is reported as not-whole and refuses too: never a guess, never a short program.
  `tests/unit/engine/frag_cabi.c` checks every stopping point of a leaf; `tests/frag_battle/adversarial_fuzz.c`
  adds regime C, which does the same under the fuzz harness.
- **Amplification is 1:1 by construction.** A load is handed no count, so there is nothing to claim four
  billion of: it allocates only as fast as the walk yields cells, and a store that wants it to allocate a lot
  must actually hold a lot. The old count-amplification probes are gone with the count they probed.
- The leaf helpers are CSE-Arena's now (`Slate::leaf_get` / `Slate::leaf_put`, taking `store_env()` and the
  arena's codec); CSE-Effect's copies are deleted.

## 0.8.0

- **Everything in the store has one shape, and a program is no exception.** A construction written out as
  bytes is a leaf, exactly like bytes handed in: it goes through the same door and lands as byte cells under
  its own word (`Arena::leaf_key` — the lens in the clear, then the bytes; one cell per byte, sign positive,
  A = the byte, B = 1). With a roster it is sliced across the carrying machines by the door in front of the
  store and gathered whole by anyone who asks for the word. It is not a row of its own kind, it is not carried
  inside an ask, and it is not a file.
- **A program's name is `word ‖ count`** — the word's bytes, then the leaf's byte count as 8 little-endian
  bytes. One spelling everywhere a program is named in a single byte string: `slate_dag_program`, the
  container's program, the tail slot, `"slate.emit"`'s answer, `"slate.run"`'s request and `"slate.tail"`'s
  hand-off. A leaf is read back by its cell count, and a word alone does not say how many cells to ask for.
  The doors that take the two apart take them as separate arguments, and so does the ask.
- **`slate_dag_save_fragment` keeps the fragment and hands back its name**, instead of writing a stream to a
  caller's sink:
  `const char *slate_dag_save_fragment(SlateDag *b, int32_t root, const uint32_t *holes, uint32_t nholes, uint8_t **word, uint64_t *wn, uint64_t *pn)`.
  `*word` is malloc'd (the caller frees it); a leaf already kept under that word is a hit and is not put
  again; a store that kept nothing refuses `"refused"` — a program nobody can read is not a program.
- **`slate_frag_load` reads that leaf back by name**, instead of pulling from a caller's source:
  `SlateFrag *slate_frag_load(SlateDag *b, const uint8_t *word, uint64_t wn, uint64_t pn)`. A word the store
  does not hold — or, on a machine carrying one share of a roster, a leaf the door could not gather whole —
  is a miss and returns NULL, never a guess. The parsed length must be exactly the leaf's.
- **`slate_invoke` takes the container and the name**:
  `SlateArray *slate_invoke(SlateDag *b, const uint8_t *word, uint64_t wn, uint64_t pn, ...)`. The container
  is the caller's, because the store is: which store holds the program is what the container says. The builder
  is no longer created and freed inside.
- **The fragment's bytes carry no marker of their own.** The leading magic and version words are gone: the
  store's word already says what the bytes are. What remains is the counts the parser reads by and a crc over
  the body — which is now what catches a store that hands back a different byte.
- **The ask carries no bytes and no tag.** New layout, little-endian, fixed order, bounds-checked
  (`embed.h` spells it): `[shares u32][k u32][roster prime i64]*k [ndims u32][dim i64]*ndims [wn u64][program
  word u8]*wn [pn u64]`. No tag byte, no version byte, and no program row: the construction does not travel,
  only its name does, and the taker reads the leaf out of its own store through the same door as any other
  row. A word without its count, or a count without its word, refuses `"args"`.
- **Removed: `slate_dag_stop` / `slate_dag_restore`** and their test. Under the one shape a checkpoint is the
  root's word — every step is a row already — so there is no door here that stops a construction to bytes and
  reads it back. The durable, position-independent token a caller carries is still the root's fragment:
  `slate_dag_save_fragment`, recovered with `slate_frag_load` + `slate_dag_splice`. (`Arena::stop`/`load`
  remain as the C++ serializer.) `slate/slate.h` no longer includes `slate/stream.h`.
- `slate_dag_program` takes a name rather than a bare word; `slate_dag_start` refuses one that is not a name.
- Tests: `tests/memstore.h` — a hashed in-memory store for the pure-C tests, and the byte-cell reader/writer a
  test needs to lie to a load. `abi_slate`, `abi_embed`, `engine/frag_cabi`, `engine/div_cabi` and every
  `frag_battle/*` keep and load through it. The hostile-input corpus now fuzzes the store rather than a file:
  one cell of a leaf is one byte of the program, and the crc is what catches a store that moves one.

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
