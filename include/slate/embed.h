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

/// Pin this region to a share of the residue prime set: it computes those channels of every value and no
/// others. A share that cannot certify a value alone refuses rather than handing back a partial one, so a
/// pinned host is never the source of a half-answer.
///
/// The share is part of a reading's name, so the same value read through two shares is two rows. That is what
/// lets several hosts each hold a share of one number without speaking to each other: each names its own share
/// and puts it, and whoever wants the whole value reads the shares back by name. This is the one setting on a
/// builder that changes which row you are asking for rather than what it costs, which is why it is here and
/// not in slate/tune.h.
///
/// `k` 0 (or a null list) leaves the region unpinned, which is the default and lets the engine choose its own
/// channels from the a-priori height.
/// @return NULL; "args" on a null builder, a null list with k > 0, or a prime outside the residue domain
///         (each must be at least 2 and below 2^24, the width the lane carries).
const char *slate_dag_lens(SlateDag *b, const int64_t *primes, uint32_t k);

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
