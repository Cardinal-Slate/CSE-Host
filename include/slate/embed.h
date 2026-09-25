/// @file embed.h
/// @brief The C ABI, embedder band: what a host that runs .slate programs installs around the record — effect
///        capabilities and hosts, the sandbox (mounts, endpoints, the deadline gate), the broker's consume
///        cursor, fiber concurrency, and the graph search. Extends slate/slate.h; a user who only builds and
///        reads records never needs it.
///
/// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "slate/slate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- OS effects (net / fs / syscalls) ----
 * An effect is a node the engine resolves to a value before any lowering, so the pure floor never performs I/O.
 * The result is a row: the request subtree is its word, so the same request anywhere dedupes to one host call
 * (a mutation must carry a distinguishing element in its bytes or it too dedupes). There is no ambient
 * authority — an effect fires only if its class is granted and a host is installed, else it refuses to a bottom
 * reading (never a wrong value). The grant is checked before the store is consulted.
 *
 * The effect is generic: the engine transports opaque request bytes to the host and opaque result bytes back —
 * it knows nothing of "read" vs "connect". The protocol — what those bytes mean — is built by a .slate fragment
 * on the DAG, not native code. Effect nodes are portable across builders — save/splice them as fragments.
 *
 * Request wire format the engine produces for the host:
 *   u32 class_len | class | u32 blob_len | blob | u32 nargs | nargs×{ i32 sign, u32 nlimbs, num LE u64s, u32 dlimbs, den LE u64s }
 * `class` is the effect class, by name (the text the node was built with); `blob` is the node's static params;
 * the args are the resolved values of the node's computed-argument children. */

/// Grant effect capabilities: the `n` class names in `classes` are the effect classes this builder may perform;
/// replaces the grant; `n` 0 = none. (An effect class is just a boundary channel, named; what the bytes on it
/// mean is protocol data, built by a .slate fragment. The engine's own classes are "slate.emit", "slate.run",
/// "slate.compose", "slate.tail".)
const char *slate_dag_effect_caps(SlateDag *b, const char *const *classes, uint32_t n);

/// Compose grant — mount a volume (Docker's -v). Expose the sandbox path `prefix` onto the real host path `real`;
/// a file open ("file:<prefix>...") is remapped through it, and an unmounted path refuses (fail-closed). `writable`
/// 0 is a read-only mount. The engine ships file + tcp providers (the available ways to open); the policy is
/// which of them may reach what.
const char *slate_dag_mount(SlateDag *b, const char *prefix, const char *real, int writable);

/// Compose grant — a tmpfs mount (Docker's --tmpfs). `prefix` is ram-backed: its files live in this container's
/// scoped store (no host path, no process global), gone when the builder is freed. `writable` 0 = read-only.
const char *slate_dag_tmpfs(SlateDag *b, const char *prefix, int writable);

/// Compose grant — expose an endpoint (Docker's -p). Permit the tcp/udp/unix provider to reach `host`:`port`;
/// `host` null or "" matches any host at that port. `listen` 0 permits a connect (client); nonzero permits a
/// bind+listen (a server — pair with the accept verb).
const char *slate_dag_expose(SlateDag *b, const char *host, uint16_t port, int listen);

/* ---- the lens, the roster and the ask: what this container computes, and what it takes to run the same
 * construction elsewhere ----
 *
 * roster   the whole lens of a word: every data prime, in order. The guard prime is the engine's, never in a
 *          roster. A word built by a region that carries a roster names the roster, not this container's primes.
 * share    the primes one machine carries: a contiguous run of the roster, roster[lo, hi). That is all the
 *          engine knows about a share, and all it needs to: every way of dividing a roster hands each unit one
 *          such run. How many units there are and which run is whose is the deployment's rule — the same on
 *          every unit, read at start and asked of the placement seam — never a setting on a container and
 *          never in a word. Re-dividing a roster therefore renames nothing.
 * ask      the bytes that let any machine run the same construction on its own share — itself a leaf, kept
 *          through the door and named by its word (slate_dag_ask); a read for that word is "run it".
 * leaf     bytes handed in, or a construction written out: one cell per byte under its own word, on the whole
 *          lens that word carries, the same shape as every other row. A program is a leaf — never a file,
 *          never bytes inside an ask.
 * walk     how a leaf is read back: word ‖ 0, word ‖ 1, … to the first cell nobody has. Nothing says how many
 *          there are — the records are the count.
 * name     how a program is carried: its word, and nothing beside it. */

/// Pin the channels this container computes: it computes those channels of every value and no others. A share
/// that cannot certify a value alone refuses rather than handing back a partial one, so a pinned host is never
/// the source of a half-answer.
///
/// With no roster set (slate_dag_roster), `primes` is the whole lens of this container's words. With a roster
/// set, `primes` must be one share of it — a contiguous sub-range roster[lo, hi), which is what any division
/// of the roster hands a unit — else the call refuses: a container never carries channels that are not a share
/// of the lens its words name. A set of the roster's primes that is not a run of them (astride, out of order,
/// with a gap) is not a share, and neither is `k` 0 with a roster set: a container carrying a roster carries
/// one of its shares. The whole roster is a share — that is the undivided deployment.
///
/// The lens is part of a reading's name, so the same value read through two lenses is two rows. That is what
/// lets several hosts each hold a share of one number without speaking to each other: each names its own share
/// and puts it, and whoever wants the whole value reads the shares back by name. This is the one setting on a
/// builder that changes which row you are asking for rather than what it costs, which is why it is here and
/// not in slate/tune.h.
///
/// `k` 0 (or a null list), with no roster set, leaves the region unpinned, which is the default and lets the
/// engine choose its own channels from the a-priori height.
/// @return NULL; "args" on a null builder, a null list with k > 0, a repeat, a prime outside the residue
///         domain — each must be at least 2, below 2^24 (the width the lane carries), actually prime, and not
///         the pool's reserved guard prime (the engine's own pool rule, applied at the door) — or a list that
///         is not one share of a roster this container carries.
const char *slate_dag_lens(SlateDag *b, const int64_t *primes, uint32_t k);

/// The whole lens this container's words name: `primes` is the roster (every data prime, in order). A
/// container with a roster computes one share of it (slate_dag_lens) and keeps share rows under the roster's
/// words, so another machine carrying another share of the same roster names the same words for the same
/// construction. The roster is the whole of what a word carries about the lens — how many units it is divided
/// into is the deployment's rule, not this container's and not a word's, which is why there is no `shares`
/// here and no way to set one. A run never spreads in process: this container computes its own share and
/// nothing else, and what carries the others is the placement seam's question, never the engine's.
///
/// Same prime rule as slate_dag_lens: each at least 2, below 2^24, actually prime, not the pool's guard, no
/// repeats. `k` 0 (or a null list) clears the roster — this container's `primes` are then the whole lens again.
/// @return NULL; "args" on a null builder, a null list with k > 0, a prime the pool's rule refuses, or a
///         roster this container's already-pinned `primes` are not a contiguous sub-range of.
const char *slate_dag_roster(SlateDag *b, const int64_t *primes, uint32_t k);

/* ---- the ask: a container's run context, kept as a leaf and named by its word ----
 *
 * The ask is what one machine hands another so it runs the same construction on its own share: the program's
 * word, the dims the last dispatch ran over, and the roster. The construction itself does not
 * travel — a program is a leaf like any bytes handed in, so with a roster it is already sliced across the
 * carrying machines and the taker walks it back through the same door it reads any other row through. Which
 * share the taker carries is the taker's own primes, handed to slate_dag_take_ask beside the ask.
 *
 * The ask travels the same way as everything else: its bytes are kept as a leaf — one cell per byte, on the
 * roster lens, under the word of (the lens in the clear ‖ the bytes) — and what the door hands back is that
 * word. The bytes never cross a door. So there are two packets and no third: the cells under a word, and a
 * row under a word. A read for an ask's word is the one thing that says "run this" (slate_dag_run_ask).
 *
 * The format is Host's, little-endian, fixed order, byte exact. Nothing rides in front of it — no tag byte,
 * no version byte:
 *
 *     [k u32][roster prime i64] * k
 *     [ndims u32][dim i64] * ndims [wn u64][program word u8] * wn
 *
 * The division is not in it, and never was in a word: how many units a roster is cut into is the deployment's
 * rule, the same on every unit, so re-dividing one renames nothing it ever named.
 *
 * `k` 0 = no roster (this container's own lens is the whole one). `wn` 0 = no program: the container names no
 * construction, and a taker of that ask has nothing to start. Nothing rides beside the word — the taker walks
 * the leaf's cells to the first one nobody has, and the records are the count. A reader bounds-checks every
 * field against the ask's end and refuses a short or malformed ask rather than guessing at the bytes. */

/// Keep this container's run context as an ask and hand back the ask's word. The bytes go through the door as
/// a leaf (one cell per byte, on the roster lens when this container carries a share of one — nothing is ever
/// kept under the bare word itself); *word/*wn is a malloc'd copy of the word, which the caller frees with
/// free(). The dims are the last dispatch's (slate_dag_run or slate_dag_start); a container that has not run
/// carries none, and the ask says ndims 0. A container that names no program asks with wn 0. Two containers
/// with the same context name the same ask — the word says what to run, never who asked.
/// @return 0; nonzero on a null argument, a store that kept nothing, or a buffer that could not be allocated.
int slate_dag_ask(SlateDag *b, uint8_t **word, uint64_t *wn);

/// Keep `bytes` as a leaf and hand back the leaf's word: the same door the ask and a fragment go through — one
/// cell per byte (sign, A = the byte, B = 1) under word ‖ 0, word ‖ 1, …, on the roster lens when this container
/// carries a share of one, sliced across the carriers by the codec it was given. The word is the lens in the clear
/// ‖ the bytes, named by that codec. Nothing is kept under the bare word, and no byte is ever kept raw. A leaf
/// already kept is a hit and is not put again. *word/*wn is a malloc'd copy, freed by the caller with free().
/// @return 0; nonzero on a null argument, no bytes, a store that kept nothing, or a buffer that could not be
///         allocated.
int slate_dag_leaf(SlateDag *b, const uint8_t *bytes, uint64_t n, uint8_t **word, uint64_t *wn);

/// Configure this container from the ask that `word` names, with `primes` as its lens — the share this machine
/// carries. The ask's leaf is walked out of this container's store (word ‖ 0, word ‖ 1, … to the first cell
/// nobody has; the records are the count), then the roster it names is set, then the lens (which must be one
/// share of that roster — a contiguous sub-range), then the program's word and the dims. Nothing is kept here: the ask
/// names the construction and carries none of it, and the program's own leaf is walked when it is started. A
/// container configured this way starts with slate_dag_start.
/// @return NULL; "partial" when the door could gather only part of the ask's leaf — a cell on some carriers
///         and not all, which is not an ask yet, never a short one; "args" on a null builder, a null word, a
///         word nothing is kept under, bytes that run past the ask's end, a roster the pool's rule refuses,
///         or `primes` that are not one share of the ask's roster.
const char *slate_dag_take_ask(SlateDag *b, const uint8_t *word, uint64_t wn, const int64_t *primes, uint32_t k);

/// Run the ask that `word` names on the share `primes`: take it, start the program it names over the dims it
/// carries, and hand back the root reading's own word (*root/*rn, malloc'd; free() it) — the key the root's
/// per-cell rows are kept under, so whoever asked walks them cell by cell. This is the whole of a unit's
/// answer to a read for an ask's word: a read for such a word is "run it".
///
/// Whether the root is whole is read off the store, never remembered: cell 0's row through the door in front
/// of this container's store. The row is there — every share of that cell landed — and the root is whole.
/// @return NULL and the root's word: whole. "share" and the root's word: this unit's share landed and the root
///         is not whole (the stopped state — another carrier has not landed its share; whoever lands the last
///         one wakes the rest). "args" on a null argument or a word that names no ask; "partial" on an ask the
///         door could only half gather; "refused" when the program does not start, or nothing anywhere is kept
///         under the root's cell.
const char *slate_dag_run_ask(SlateDag *b, const uint8_t *word, uint64_t wn, const int64_t *primes, uint32_t k,
                              uint8_t **root, uint64_t *rn);

/// The reading's own word: the key its per-cell rows are kept under. Hand it to the door to walk the reading
/// cell by cell (word ‖ 0, word ‖ 1, …), which is what a run hands back (slate_dag_run_ask). Writes a malloc'd
/// buffer to *out/*n; the caller frees it with free().
/// @return NULL; "args" on a null argument or a reading that carries no word (a refused dispatch).
const char *slate_array_word(const SlateArray *a, uint8_t **out, uint64_t *n);

/// The word of cell `i` of a reading: the reading's own word with the cell's index after it, the same name the
/// engine's per-cell rows are kept under. A deployment hands this to the door in front of the store to ask
/// whether the root is whole — whether every share of that cell has landed — without the engine being asked
/// anything. Writes a malloc'd buffer to *out/*n; the caller frees it with free().
/// @return NULL; "args" on a null argument or a reading that carries no word (a refused dispatch).
const char *slate_array_cell_word(const SlateArray *a, uint64_t i, uint8_t **out, uint64_t *n);


/// The program this container runs: the word of a leaf, and nothing beside it (slate_dag_save_fragment hands
/// that word back; the "slate.emit" answer and the "slate.tail" hand-off carry the same one thing). Context,
/// like the lens — not a node. slate_dag_start runs it through the run trampoline: the leaf is walked out of
/// the store — word ‖ 0, word ‖ 1, … to the first cell nobody has — spliced into a fresh arena sharing this
/// container (grant / fds / providers), run, and if it hands off with "slate.tail" the next word is run the
/// same way, until a tick hands off to nothing. `n` 0 clears it.
///
/// A container that carries a roster sets this itself if nothing else has: at the first slate_dag_run with a
/// roster set and no program, the construction being run is kept as a leaf (its bytes, one cell per byte, under
/// their own word) and that word becomes the program — so the ask this container hands another machine names
/// the construction rather than carrying it. A construction that cannot be saved as a fragment (an RNS/ℚ, f32
/// or wide carrier is not fragment-addressable — see slate_dag_save_fragment), or one no store would keep, is
/// not kept, the run is unaffected, and the ask then names no program. Clear the word to let the next dispatch
/// name a new one.
/// @return NULL; "args" on a null builder or a null word with n > 0.
const char *slate_dag_program(SlateDag *b, const uint8_t *word, uint64_t n);
/// Start the container: run the program its context names (slate_dag_program) over `dims`. Returns the reading
/// of the last tick, or NULL when no program is set, the store does not hold the leaf the word names (or holds
/// only part of it), or a tick refuses. An entrypoint is a store, a secret and a word; nothing in it is a graph. `dims` NULL with `ndims` 0 runs over
/// the dims the container already carries — the ones an ask brought (slate_dag_take_ask) or the last dispatch
/// recorded — and refuses when it carries none.
SlateArray *slate_dag_start(SlateDag *b, const int64_t *dims, uint32_t ndims);

/// Install the deadline/cancel gate. `proceed(user)` is called at each effect boundary; return nonzero to proceed,
/// 0 to abort the effect (a clean refusal to a bottom). This enforces a wall-clock deadline, a cpu/effect budget,
/// or cancellation of a long-running server loop — the engine never reads a clock itself; the embedder owns the
/// policy behind this callback. Pass null to clear it.
const char *slate_dag_proceed(SlateDag *b, int (*proceed)(void *user), void *user);

/// Install the host effect vtable. `perform` runs an effect synchronously (blocks); it writes a malloc'd result
/// buffer to *out/*outn (the engine frees it) and returns 0, or nonzero to fail the effect. `begin` (optional)
/// starts an effect asynchronously and returns 0, then the host calls slate_effect_complete(slot, data, n) when
/// ready — used only when the resolver runs on a fiber, otherwise `perform` is used. `user` is passed through to
/// both. Pass null for a callback not offered.
const char *slate_dag_effect_host(SlateDag *b,
    int (*perform)(const char *cls, const uint8_t *req, uint64_t reqn, uint8_t **out, uint64_t *outn, void *user),
    int (*begin)(const char *cls, const uint8_t *req, uint64_t reqn, void *slot, void *user), void *user);

/// Build an effect node: `cls` (the class, by name), static request params [blob, blob+blen) interpreted by
/// the host, and `nargs` computed-argument child node ids (each forced to a value at resolve time and folded
/// into the request). Returns the node id, or -1 on bad args.
int32_t slate_dag_effect(SlateDag *b, const char *cls, const uint8_t *blob, uint64_t blen,
                         const int32_t *args, uint32_t nargs);

/// Build an array-effect: the byte/message (consume) form. Performs the effect at dispatch and fills a reserved
/// carrier with `len` byte-cells (the result bytes, one per cell, zero-padded to `len`). Returns the carrier id
/// (read cells with slate_dag_load), or UINT32_MAX on bad args. This is the broker consume side —
/// read(handle, offset, len) → an array the DAG indexes; total size comes from paginating over offsets, never a
/// wide value. `args` are the computed effect arguments (e.g. offset, len); `blob` the static params (e.g. the
/// handle/path). Scalar values (fd, size, status) use slate_dag_effect instead.
uint32_t slate_dag_effect_array(SlateDag *b, const char *cls, const uint8_t *blob, uint64_t blen,
                                const int32_t *args, uint32_t nargs, int64_t len);

/// A zero-copy view of one effect operand handed to a receipt-operand io host: a carrier's positional-int plane.
/// Read element i as an `elem_bytes`-wide little-endian value; a byte carrier is width 1, so ((const uint8_t*)
/// plane)[i] is byte i. Borrowed — valid only for the perform callback.
typedef struct SlateOperand { const void *plane; uint32_t elem_bytes; uint64_t n; } SlateOperand;

/// Install the receipt-operand io host: interior operands cross by reference as `ops` (carrier planes), not
/// marshalled — the operand is the index/pointer (e.g. the address); only the resource content is bytes. On
/// success write a malloc'd result to *out/*outn (the engine fills a carrier — the verdict) and return 0.
const char *slate_dag_effect_host_io(SlateDag *b,
    int (*perform)(const char *cls, const uint8_t *blob, uint64_t blen, const SlateOperand *ops, uint32_t nops,
                   uint8_t **out, uint64_t *outn, void *user), void *user);

/// Build a receipt-operand io effect: `cls` (by name), static `blob` (the op), and `noperands` carrier ids
/// passed by reference. Fills a reserved carrier of `out_len` cells with the result (read with slate_dag_load) —
/// the verdict. Returns the out carrier id, or UINT32_MAX. Composite is receipt→receipt: pass this out carrier id
/// as another effect_io operand. An open verb's result is the word of that open effect — the name of the opened
/// resource — and the stream verbs (read/write/close/accept) take that word as ops[0]; the fd never crosses.
uint32_t slate_dag_effect_io(SlateDag *b, const char *cls, const uint8_t *blob, uint64_t blen,
                             const uint32_t *operand_carriers, uint32_t noperands, int64_t out_len);

/// consume: a broker read whose cursor is a word, not an offset. Reads up to `len` bytes of the resource `handle`
/// names, after the chunk `cursor` names — the word a previous slate_dag_cursor returned; NULL/0 = the start —
/// into a carrier (returned cid; read cells with slate_dag_load). After slate_dag_run, slate_dag_cursor gives this
/// chunk's word to present next. The position is resolved from the chunk's own row (in the store, or this
/// container's table); it never crosses. Returns UINT32_MAX on bad args.
uint32_t slate_dag_consume(SlateDag *b, const char *cls, const uint8_t *handle, uint64_t hlen,
                           const uint8_t *cursor, uint64_t cn, int64_t len);

/// The cursor of a consume `cid` (see slate_dag_consume), valid after slate_dag_run: `word` names the chunk just
/// read (borrowed from the builder, valid until its next run or free) and `closure` is "closed" (drained: a short
/// read hit the end) or "incomplete" (more to read). Carry the word; present it as the next consume's cursor.
typedef struct { const uint8_t *word; uint64_t wn; const char *closure; } slate_cursor;
/// Returns "refused" if not yet run, or not a consume.
const char *slate_dag_cursor(SlateDag *b, uint32_t cid, slate_cursor *out);

/* ---- fiber concurrency (pure C): overlap many async effects on one thread pool ----
 * An async host (slate_dag_effect_host with a `begin`) only parks — instead of blocking a worker — when the
 * work runs on a fiber. These entries put you on one: run a body on the pool, spawn concurrent tasks inside it.
 * Each task's slate_dag_run parks on its begin effects; slate_effect_complete wakes them. No C++ needed. */

/// Opaque handle to a running scope (valid only inside a slate_scope_run body).
typedef struct SlateScope SlateScope;

/// Run `body` on the fiber pool; blocks the caller until the scope's spawned tasks join. Inside `body`, use
/// `scope` to spawn concurrent work.
void slate_scope_run(void (*body)(SlateScope *scope, void *user), void *user);

/// Spawn `task` as a concurrent fiber in `scope`; joined before slate_scope_run returns. A task that calls
/// slate_dag_run parks (not blocks) on async effects, so many tasks overlap their waits on one thread pool.
void slate_scope_spawn(SlateScope *scope, void (*task)(void *user), void *user);

/// The host's async-effect completion callback: call from any thread when a slate_dag_effect_host `begin` op
/// finishes, passing the `slot` begin received and the `n` result bytes. Wakes the parked consumer.
void slate_effect_complete(void *slot, const uint8_t *data, uint64_t n);

/* ---- search: the S axis over a graph — one door, measurement (by shape) or value (by computation) ---- */

/// The unified search over the graph rooted at `root`. `operation` selects which of the two searches runs:
///
///   • operation < 0  → measurement (by shape). Find every node reachable from `root` (walking its operands)
///     whose carried shape matches the target {potential P, mode Λ}. A structural relatedness query: it matches
///     on the shape the builder already composed onto each node — not on value, not on node id. `potential` is
///     the height bound P (a node's `h_bits`); `mode` is Λ by name, "compose" or "decompose" (NULL = compose).
///     No value is computed: `out_num`/`out_den`/`out_undef` stay at defaults.
///
///   • operation >= 0 → value (by computation). Run the value search over the collection (a plain op(left,right)
///     composition, elements at the leaves in monotone order). `operation` is a node whose Λ steers, read off
///     the graph: a `sub`-built op (decompose) locates the element equal to `target` (the certified sign of
///     target−element bisects the tree in place, sub-linear); an `add`-built op (compose) aggregates the exact
///     fold of every element (pass `target` = a lit(0) node). `target` names the value sought (a lit node),
///     unless `extremum` is "min" or "max": then the decompose descends to the collection's own least / greatest
///     element (sub-linear); NULL = by `target`. The settled value is decoded into `*out_num`/`*out_den`; a
///     refused/undefined value sets `*out_undef=1` (with NULL); a value past int64 returns "wide".
///
/// In both modes: matched node ids are written to `out[0 .. min(*out_count, cap))`; `*out_count` receives the
/// TOTAL match count, which may exceed `cap` — re-call with a larger `out` to read them all. `*out_visited`
/// receives the number of vertices walked; `*out_resumable` is 1 if `budget` (0 = unbounded) cut the walk
/// before it finished. Pass NULL for any out you do not want.
/// @return NULL; "wide" if a by-value result exceeds int64; "args" on a null/poisoned builder or an out-of-range
///         `root` (or, for a by-value query, `target`/`operation`), or a `mode`/`extremum` not listed above.
const char *slate_dag_search(SlateDag *b, int32_t root, uint64_t potential, const char *mode, uint64_t budget,
                     int32_t *out, uint32_t cap, uint32_t *out_count, uint64_t *out_visited,
                     int32_t *out_resumable, int32_t target, const char *extremum, int32_t operation,
                     int64_t *out_num, int64_t *out_den, int32_t *out_undef);

#ifdef __cplusplus
}
#endif
