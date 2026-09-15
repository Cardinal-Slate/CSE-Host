/// @file array.h
/// @brief C ABI for the array front door: DagBuild across C — the minimal basis (carriers, index
///        arithmetic, reductions) dispatched over a product grid.
///
/// SPDX-License-Identifier: MIT OR Apache-2.0
///
/// `Slate::DagBuild` (dag/build.hpp) with each node handle carried across the C boundary as the int it already
/// is. Only node ids cross the ABI; every array operation (map, gather, transpose, matmul, conv, dot) is built
/// from these calls in the caller's language, never bound one-by-one. A builder holds an Arena DAG; `run`
/// dispatches the DAG rooted at a node over the product grid `dims` and returns a result owning the grid of
/// cells. Legs: `carrier`/`load` are the arrays; `param` + index arithmetic over `dims` is shape and position.
///
/// Ownership & threading: a builder and its results are caller-owned (free with slate_dag_free /
/// slate_array_free). An Arena is single-threaded — a SlateDag must not be shared across threads; a SlateArray,
/// once produced, is an immutable owned buffer readable from any thread. Return codes reuse the SLATE_BATCH_*
/// set in slate/batch.h. Same engine dispatch (dag/lower/install.hpp) the C++ surface runs; shares nothing with
/// the flat slate_map fold path.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "slate/batch.h"    /* the shared SLATE_BATCH_* return codes */
#include "slate/stream.h"   /* slate_sink / slate_source — the byte-stream callbacks for stop / restore */

#ifdef __cplusplus
extern "C" {
#endif

/// An opaque DAG builder (owns an Arena). Build leaves and ops into it, then slate_dag_run.
typedef struct SlateDag SlateDag;

/// An opaque array result (owns the grid of cells produced by one slate_dag_run). Outlives its builder.
typedef struct SlateArray SlateArray;

/// An opaque loaded fragment: a serialized DAG piece with typed holes (a .slate library). Immutable once
/// loaded, so it is shareable across threads and spliceable into any builder. Free with slate_frag_free.
typedef struct SlateFrag SlateFrag;

/// Creates an empty builder. Returns NULL only if the engine could not allocate. Free with slate_dag_free.
SlateDag *slate_dag_new(void);

/// Creates a builder whose Arena reserves `arena_bytes` up front (0 = the default reserve). A larger reserve
/// avoids arena growth for a big construction. Returns NULL only if the engine could not allocate.
SlateDag *slate_dag_new_sized(uint64_t arena_bytes);

/// Frees a builder and its Arena. Results already produced by slate_dag_run stay valid (they own their cells).
void slate_dag_free(SlateDag *b);

/* ---- scope: the measurement context slate_dag_run dispatches under ---- */
/* Each setter patches the builder's scope (the same Envelope the C++ Region guards patch); slate_dag_run
 * activates it for the dispatch and restores it after. A knob chooses who computes or the a-priori height it
 * computes to, never the value, so a result stays exact under any setting. Setters compose (last write per
 * axis wins) and return SLATE_BATCH_OK, or SLATE_BATCH_EARGS on a null builder / out-of-range argument. */

/// Declares the Potential — the RNS value-model height ceiling in bits — so a carried (rational) result
/// provisions enough channels for the whole computation's height. Auto-derived from rational carriers when
/// left unset; the larger of the two applies.
int slate_dag_potential(SlateDag *b, uint32_t bits);

/// Caps host fan-out at `n` threads (0 = hardware concurrency). A cost knob: it changes timing, not the result.
int slate_dag_threads(SlateDag *b, uint32_t n);

/// Routes the dispatch to the device (GPU) executor when a backend is linked. A no-op with no backend, and the
/// driver sample-checks the device result and falls back to the host on any mismatch, so it never changes a value.
int slate_dag_device(SlateDag *b, int on);

/// Forces the leaf JIT on (1) or off (0). Off runs the interpreter — for an A/B baseline against the compiled lane.
int slate_dag_jit(SlateDag *b, int on);

/// Forces leaf vectorisation on (1) or off (0). Off keeps the per-item lane, the same result more slowly.
int slate_dag_vec(SlateDag *b, int on);

/// Sets the telemetry level (0 off, 1 timing, 2 profile). Pair with slate_dag_trace to install a destination;
/// with no sink the engine gates the emit sites off and pays nothing.
int slate_dag_telemetry(SlateDag *b, int level);

/// Installs the telemetry sink `emit` (with `user`) that receipts stream to; NULL clears it. The builder holds
/// the sink for its lifetime, so it must outlive the runs that emit through it.
int slate_dag_trace(SlateDag *b, slate_emit emit, void *user);

/// Sets the real-read bracket width target (2^-prec_bits) — how tightly a real value is resolved.
int slate_dag_width(SlateDag *b, uint32_t prec_bits);

/// Caps a real read's refine iterations (0 = the loop's own cap). Bounds work on a value that refines forever.
int slate_dag_work(SlateDag *b, uint64_t cap);

/// Caps per-read scratch at `bytes`; a real read that would exceed it refuses rather than exhausting memory.
int slate_dag_mem(SlateDag *b, uint64_t bytes);

/// Constrains RNS channel selection: `pin` forces one ChannelType (0 Auto, 1 I16, 2 I32, 3 I64, 4 F32, 5 F64),
/// `allow_mask` is a bitmask of permitted types. A cost/routing knob over the residue lanes, never the value.
int slate_dag_channel(SlateDag *b, int pin, uint32_t allow_mask);

/// Sets the RNS channel cost model — the coefficients the planner ranks lanes by. `gmacs` and `dispatch_ms`
/// are arrays of 6 doubles indexed by ChannelType {Auto, I16, I32, I64, F32, F64} (giga-MACs/sec throughput
/// and fixed per-channel dispatch ms); a NULL array leaves that set at its default. `decomp_narrow` /
/// `decomp_wide` are per-element operand-decompose costs in ms for operands ≤int64 / bignum; a negative value
/// leaves that coefficient unchanged. Calibration for cross-hardware benchmarking — it moves which lane the
/// planner picks, never the value.
int slate_dag_channel_costs(SlateDag *b, const double *gmacs, const double *dispatch_ms,
                            double decomp_narrow, double decomp_wide);

/* ---- leaves ---- */

/// A grid coordinate: the value of dispatch axis `slot` (0-based into the `dims` passed to run) at each cell.
int32_t slate_dag_param(SlateDag *b, uint32_t slot);

/// An integer constant leaf.
int32_t slate_dag_lit(SlateDag *b, int64_t v);

/// Reflection: read one node's structure off the graph (the engine's own lower_view). A fragment spliced into a
/// builder is a graph of these — walking ids 0..root recovers its exact construction (op + child ids + leaf
/// value), so a caller can see structurally what a fragment is composed of, with no collision. `*kind`: 0 op,
/// 1 shard, 2 lit, 3 carrier, 4 big. For an op, `*op` is the opcode and `*a`/`*bchild` are child node ids (-1 if
/// absent); for a lit, `*lit_n`/`*lit_d` are the value. Pass NULL for any out you do not want.
/// @return SLATE_BATCH_OK; SLATE_BATCH_EARGS on a null builder or an out-of-range/invalid `id`.
int slate_dag_node(SlateDag *b, int32_t id, int32_t *kind, int32_t *op,
                   int32_t *a, int32_t *bchild, int64_t *lit_n, int64_t *lit_d);

/// Registers an array that crosses the boundary: `n` int64 values, copied into the builder. Returns its
/// carrier id (for slate_dag_load), or UINT32_MAX on bad args (null builder/vals with n>0).
uint32_t slate_dag_carrier(SlateDag *b, const int64_t *vals, uint64_t n);

/// Registers a rational array cleared to a common denominator: cell i = `nums[i]` / `den` (one shared
/// denominator), folded into residues so a reduce stays an integer channel-dot and the readback Wang-recovers
/// each cell's num/den. `hbits` is the whole computation's result height (numerator + denominator bits, with
/// margin) — provision it to the result, not the operand, so an under-provisioned cell decodes to undefined,
/// never a wrong value. All rational carriers in one build should pass the same `hbits`; slate_dag_run scopes
/// its Potential to the max. Returns its carrier id, or UINT32_MAX on bad args (`den` == 0, negative `hbits`,
/// null builder/nums). Mixing rational and plain int64 carriers in one build is not supported.
uint32_t slate_dag_carrier_q(SlateDag *b, const int64_t *nums, uint64_t n, int64_t den, int32_t hbits);

/// carrier[cid] read at node `idx` (any index expression: a param, an rvar, or arithmetic over them).
int32_t slate_dag_load(SlateDag *b, uint32_t cid, int32_t idx);

/* ---- ops (the locked fold-dialect op codes) ---- */

int32_t slate_dag_add(SlateDag *b, int32_t x, int32_t y);
int32_t slate_dag_mul(SlateDag *b, int32_t x, int32_t y);
int32_t slate_dag_sub(SlateDag *b, int32_t x, int32_t y);
/// Signed high-multiply (smulh): the high 64 bits of x*y. The decompose-family basis (floor-div,
/// reduce-to-canonical) is built from mulh + asr + sub, so exposing them lets those libraries be
/// expressed through the C ABI — not just the map/matmul shape.
int32_t slate_dag_mulh(SlateDag *b, int32_t x, int32_t y);
/// Arithmetic shift right of `x` by `sh` (an integer-constant leaf, i.e. from slate_dag_lit).
int32_t slate_dag_asr(SlateDag *b, int32_t x, int32_t sh);

/* ---- OS effects (net / fs / syscalls) ----
 * An effect is a node the engine resolves to a value before any lowering, so the pure floor never performs I/O.
 * The result is content-hash journaled (the request subtree is the ETag, no TTL): the same request dedupes to
 * one host call; a mutation must carry a distinguishing element in its bytes or it too dedupes. There is no
 * ambient authority — an effect fires only if its class bit is granted and a host is installed, else it refuses
 * to a bottom reading (never a wrong value).
 *
 * The effect is generic: the engine transports opaque request bytes to the host and opaque result bytes back —
 * it knows nothing of "read" vs "connect". It is just a message broker marshalling data at the boundary: bytes
 * in, bytes out. The protocol — what those bytes mean, how a request is formed and a reply read — is built by a
 * .slate fragment on the DAG, not native code. Effect nodes are portable across builders — save/splice them as
 * .slate fragments.
 *
 * Request wire format the engine produces for the host:
 *   u32 class | u32 blob_len | blob | u32 nargs | nargs×{ i32 sign, u32 nlimbs, num LE u64s, u32 dlimbs, den LE u64s }
 * `class` is the effect class (== the capability bit index); `blob` is the node's static params; the args are
 * the resolved values of the node's computed-argument children. */

/// Grant effect capabilities: `mask` bit (class & 63) set = that effect class is permitted. 0 = none. (An effect
/// class is just a boundary channel; what the bytes on it mean is protocol data, built by a .slate fragment.)
int slate_dag_effect_caps(SlateDag *b, uint64_t mask);

/// Compose grant — mount a volume (Docker's -v). Expose the sandbox path `prefix` onto the real host path `real`;
/// a file open ("file:<prefix>...") is remapped through it, and an unmounted path refuses (fail-closed). `writable`
/// 0 is a read-only mount. This is config, the same paradigm as slate_dag_effect_caps; a .slate compose spec drives
/// it. The engine ships file + tcp providers (the available ways to open); the policy is which of them may reach what.
int slate_dag_mount(SlateDag *b, const char *prefix, const char *real, int writable);

/// Compose grant — a tmpfs mount (Docker's --tmpfs). `prefix` is ram-backed: its files live in this container's
/// scoped in-memory store (no host path, no process global), gone when the builder is freed. `writable` 0 = read-only.
int slate_dag_tmpfs(SlateDag *b, const char *prefix, int writable);

/// Compose grant — expose an endpoint (Docker's -p). Permit the tcp/udp/unix provider to reach `host`:`port`;
/// `host` null or "" matches any host at that port. `listen` 0 permits a connect (client); nonzero permits a
/// bind+listen (a server — pair with the accept verb).
int slate_dag_expose(SlateDag *b, const char *host, uint16_t port, int listen);

/// Install the deadline/cancel gate. `proceed(user)` is called at each effect boundary; return nonzero to proceed,
/// 0 to abort the effect (a clean refusal to a bottom). This enforces a wall-clock deadline, a cpu/effect budget,
/// or cancellation of a long-running server loop — the engine never reads a clock itself; the embedder owns the
/// policy behind this callback. Pass null to clear it.
int slate_dag_proceed(SlateDag *b, int (*proceed)(void *user), void *user);

/// Install the host effect vtable. `perform` runs an effect synchronously (blocks); it writes a malloc'd result
/// buffer to *out/*outn (the engine frees it) and returns 0, or nonzero to fail the effect. `begin` (optional)
/// starts an effect asynchronously and returns 0, then the host calls slate_effect_complete(slot, data, n) when
/// ready — used only when the resolver runs on a fiber, otherwise `perform` is used. `user` is passed through to
/// both (the reference host reads it as an optional syscall allow-list). Pass null for a callback not offered.
int slate_dag_effect_host(SlateDag *b,
    int (*perform)(uint32_t id, const uint8_t *req, uint64_t reqn, uint8_t **out, uint64_t *outn, void *user),
    int (*begin)(uint32_t id, const uint8_t *req, uint64_t reqn, void *slot, void *user), void *user);

/// Install the codec and store on this builder's region: `encode` (bytes → the word), `decode` (the word → the
/// bytes under it, or nonzero = not there) and `put` ((word, bytes) out), each with the perform contract (malloc'd
/// *out/*outn, return 0), and `secret` — the shared secret, `sn` bytes, copied — handed to all three. Pass null
/// for a callback not offered: encode defaults to a generic hash of the bytes keyed by the secret; decode/put
/// default to the region's in-process store. Everything the region computes — readings, compiled programs,
/// effects, residue promotions — is looked up by its word before it runs and put after. The region's tasks inherit it.
int slate_dag_codec(SlateDag *b,
    int (*encode)(const uint8_t *bytes, uint64_t n, const uint8_t *secret, uint64_t sn, uint8_t **out, uint64_t *outn, void *user),
    int (*decode)(const uint8_t *word, uint64_t wn, const uint8_t *secret, uint64_t sn, uint8_t **out, uint64_t *outn, void *user),
    int (*put)(const uint8_t *word, uint64_t wn, const uint8_t *bytes, uint64_t n, const uint8_t *secret, uint64_t sn, void *user),
    const uint8_t *secret, uint64_t sn, void *user);

/// Build an effect node: `class` (the capability domain), static request params [blob, blob+blen) interpreted by
/// the host, and `nargs` computed-argument child node ids (each forced to a value at resolve time and folded
/// into the request). Returns the node id, or -1 on bad args. slate_effect_complete (the async completion
/// callback) is provided by the C++ async runtime (slate/async/effect_bridge.hpp), since async parking requires
/// the fiber runtime to be linked.
int32_t slate_dag_effect(SlateDag *b, uint32_t cls, const uint8_t *blob, uint64_t blen,
                         const int32_t *args, uint32_t nargs);

/// Build an array-effect: the byte/message (consume) form. Performs the effect at dispatch and fills a reserved
/// carrier with `len` byte-cells (the result bytes, one per cell, zero-padded to `len`). Returns the carrier id
/// (read cells with slate_dag_load), or UINT32_MAX on bad args. This is the broker consume side —
/// read(handle, offset, len) → an array the DAG indexes; total size comes from paginating over offsets, never a
/// wide value. `args` are the computed effect arguments (e.g. offset, len); `blob` the static params (e.g. the
/// handle/path). Scalar values (fd, size, status) use slate_dag_effect instead.
uint32_t slate_dag_effect_array(SlateDag *b, uint32_t cls, const uint8_t *blob, uint64_t blen,
                                const int32_t *args, uint32_t nargs, int64_t len);

/// A zero-copy view of one effect operand handed to a receipt-operand io host: a carrier's positional-int plane.
/// Read element i as an `elem_bytes`-wide little-endian value; a byte carrier is width 1, so ((const uint8_t*)
/// plane)[i] is byte i. Borrowed — valid only for the perform callback.
typedef struct SlateOperand { const void *plane; uint32_t elem_bytes; uint64_t n; } SlateOperand;

/// Install the receipt-operand io host: interior operands cross by reference as `ops` (carrier planes), not
/// marshalled — the operand is the index/pointer (e.g. the address); only the resource content is bytes. On
/// success write a malloc'd result to *out/*outn (the engine fills a carrier — the verdict) and return 0.
int slate_dag_effect_host_io(SlateDag *b,
    int (*perform)(uint32_t id, const uint8_t *blob, uint64_t blen, const SlateOperand *ops, uint32_t nops,
                   uint8_t **out, uint64_t *outn, void *user), void *user);

/// Build a receipt-operand io effect: `cls`, static `blob` (the op), and `noperands` carrier ids passed by
/// reference. Fills a reserved carrier of `out_len` cells with the result (read with slate_dag_load) — the
/// verdict. Returns the out carrier id, or UINT32_MAX. Composite is receipt→receipt: pass this out carrier id
/// as another effect_io operand.
uint32_t slate_dag_effect_io(SlateDag *b, uint32_t cls, const uint8_t *blob, uint64_t blen,
                             const uint32_t *operand_carriers, uint32_t noperands, int64_t out_len);

/// consume: a broker read whose cursor is a Receipt, not a bare offset. Reads up to `len` bytes at `offset` from
/// `handle` into a carrier (returned cid — read cells with slate_dag_load); `offset` is just this request's
/// addressing. After slate_dag_run, call slate_dag_cursor (declared with slate_reading, below) for the cursor.
/// Returns UINT32_MAX on bad args.
uint32_t slate_dag_consume(SlateDag *b, uint32_t cls, const uint8_t *handle, uint64_t hlen,
                           int64_t offset, int64_t len);

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

/// Dispatches the DAG rooted at `root` over the product grid `dims` (ndims axes), returning a result of
/// ∏dims cells. Returns NULL on a refused dispatch: a malformed builder (a bad node/carrier id was passed
/// earlier), a non-lowerable leaf, or a cell whose exact value exceeds int64 (the wide/RNS path is a
/// follow-on) — never a wrong value. The builder may be freed or reused (build a new root) after this call.
SlateArray *slate_dag_run(SlateDag *b, int32_t root, const int64_t *dims, uint32_t ndims);

/// Source-in for a reader: register raw bytes directly as a carrier (min width, one bulk copy — no int64
/// widening). A reader's source is bytes; the fast text-source door. Returns a carrier id.
uint32_t slate_dag_carrier_bytes(SlateDag *b, const uint8_t *bytes, uint64_t n);
/// Swap carrier `cid`'s bytes in place (the byte fast path for the U/E split). Returns SLATE_BATCH_OK or SLATE_BATCH_EARGS.
int slate_dag_carrier_set_bytes(SlateDag *b, uint32_t cid, const uint8_t *bytes, uint64_t n);

/// U/E split: replace carrier `cid`'s data in place, then re-run the same construction over the new
/// source — no rebuild, no recompile (the compiled leaf is carrier-independent). Compile the reader once
/// (build + one run), then feed many sources via this at dispatch speed. Returns SLATE_BATCH_OK or SLATE_BATCH_EARGS.
int slate_dag_carrier_set(SlateDag *b, uint32_t cid, const int64_t *vals, uint64_t n);

/* ---- persist / resume: stream a construction over the byte callbacks (wraps Arena stop/load) ---- */

/// Serializes the builder's whole construction (nodes + literal pool) to `sink` — the .slate checkpoint —
/// and sheds its memory. After stop the builder refuses ops until slate_dag_restore; a result already
/// produced by slate_dag_run stays valid (it owns its cells). Returns SLATE_BATCH_OK, SLATE_BATCH_EARGS on
/// null, or SLATE_BATCH_EINTERNAL if the stream refused.
int slate_dag_stop(SlateDag *b, slate_sink sink, void *user);

/// Restores a construction serialized by slate_dag_stop from `source`, resuming the same builder: the node
/// table returns 1:1 so pre-stop ids still resolve (a resume convenience) and a budget-stopped (Incomplete)
/// reading resumes where it left off. A raw node id is not the durable identity, though — it does not
/// survive a fresh start(). The handle to carry a computation across a checkpoint is the root's save_fragment
/// trajectory token: slate_dag_save_fragment(root) captured before force(), recovered by slate_frag_load +
/// slate_dag_splice into a fresh root (see the persist/resume note in engine_cabi.cpp for the
/// addressable-carrier caveats). Returns SLATE_BATCH_OK, SLATE_BATCH_EARGS, or SLATE_BATCH_EINTERNAL.
int slate_dag_restore(SlateDag *b, slate_source source, void *user);

/* ---- fragment libraries: a .slate file is a serialized DAG piece with typed holes -------------------------
 *
 * A fragment is a construction with unbound carriers (holes) — a reusable graph piece, not a finished
 * computation. save_fragment records the sub-DAG rooted at a node as normalized builder instructions (the
 * portable source form, never lowered nodes — each host re-lowers in its own context); load parses it back;
 * splice replays it into another builder, wiring the caller's carriers into its holes and returning the new
 * root node id. Receipts compose at the seam, so linking is receipt-in / receipt-out. It is bytes, so any
 * host linking libslate loads and splices the same file. This is distinct from the stop/restore checkpoint:
 * that is a same-build snapshot of one builder; a fragment is an archival, cross-builder library.
 *
 * The fragment is the interchange unit for a computation, not data, with two properties. Interoperable: the
 * interchange is the graph itself — authored in any surface (C++, or any binding) and loaded, iface-checked,
 * and spliced from any other (write in C++, run from Python). Idempotent: evaluation is referentially
 * transparent over canonical Q, so a spliced fragment's record is a pure function of the graph — save then
 * load then run reproduces it on any host or backend, or refuses. Determinism is what makes it portable. */

/// A fragment interface receipt — the *signature* of a hole or the root (what it expects / produces), as
/// opposed to slate_reading (what a finished dispatch produced).
typedef struct {
  int32_t  kind;    /* 0 scalar (a per-cell value: the root), 1 array/carrier (a hole) */
  int32_t  domain;  /* expected tower set: 0 ℕ 1 ℤ 2 ℚ 3 ℝ 4 ℂ; -1 = derive / any */
  int32_t  mode;    /* Λ: 0 compose, 1 decompose; -1 = any */
  uint64_t h_bits;  /* the Potential (height in bits) the fragment provisions for; 0 = derive */
} slate_iface_receipt;

/// One unbound carrier (a hole the caller must fill at splice time).
typedef struct {
  uint32_t slot;                /* the hole's index — pass its carrier at arg_carriers[slot] in slate_dag_splice */
  slate_iface_receipt receipt;  /* what the caller must supply */
} slate_hole;

/// Serialize the sub-DAG rooted at `root` to `sink` as a fragment, declaring the carrier ids in `holes`
/// (nholes of them) as unbound inputs. Carriers referenced by `root` but not listed in `holes` are baked in
/// as constants (their int64 cell values are serialized) — a baked carrier must be a plain positional
/// integer array (an RNS/float carrier must be a hole). The instruction stream is the portable source form,
/// so a spliced fragment re-lowers in the consumer's context.
/// @return SLATE_BATCH_OK; SLATE_BATCH_EARGS (null/poisoned builder, bad root, a hole id that is not a carrier
///         `root` reads, or a non-serializable baked carrier); SLATE_BATCH_EINTERNAL (the sink refused).
int slate_dag_save_fragment(SlateDag *b, int32_t root, const uint32_t *holes, uint32_t nholes,
                            slate_sink sink, void *user);

/// Parse a fragment from `source`. Fully validated before it is usable (magic/version, back-reference-only
/// child ids so the graph is acyclic, in-range carrier/param references, bounded sizes, a crc over the body),
/// so a malformed or hostile stream returns NULL rather than a bad fragment. Caller owns; free with
/// slate_frag_free. The fragment is immutable and may be spliced into many builders and read from any thread.
SlateFrag *slate_frag_load(slate_source source, void *user);

/// Report the interface: the number of grid params, the holes and their receipts, and the root receipt. Pass
/// NULL for any out you do not want. Size-then-fill for the holes: pass `holes_out` NULL to read the count in
/// `*nholes_io`, then again with a buffer of that many entries.
/// @return SLATE_BATCH_OK; SLATE_BATCH_EARGS on a null fragment; EOUTSIZE if `holes_out` is non-NULL and `*nholes_io` is
///         smaller than the hole count (`*nholes_io` is set to the needed count).
int slate_frag_iface(const SlateFrag *f, uint32_t *nparams, slate_hole *holes_out, uint32_t *nholes_io,
                     slate_iface_receipt *root_out);

/// Splice fragment `f` into builder `b`: replay its instructions (remapping its node ids into b's id space
/// and composing receipts at the seam), registering its baked carriers into b and wiring the caller's
/// carriers into its holes. `arg_carriers[i]` is the carrier id (already registered in b) for hole i, in the
/// order slate_frag_iface reports; `narg` must equal the hole count. A hole whose supplied carrier's domain
/// is incompatible with the declared receipt refuses before any node is emitted, so b is left unmodified.
/// @return the new root node id (>= 0) in b, or a negative -(SLATE_BATCH_E*) code: -SLATE_BATCH_EARGS
///         (null/arity/id), -EREFUSED (a hole failed its receipt typecheck), -SLATE_BATCH_EINTERNAL.
int32_t slate_dag_splice(SlateDag *b, const SlateFrag *f, const uint32_t *arg_carriers, uint32_t narg);

/// Free a loaded fragment. Node results and builders already produced from a splice stay valid (splice copies).
void slate_frag_free(SlateFrag *f);

/// The executable invariant, as one call. Load a fragment from `frag`/`frag_len`, register each byte-arg as a
/// carrier and wire them into the fragment's holes (in slate_frag_iface order; `nargs` must equal the hole
/// count), run the spliced root over `dims`/`ndims`, and return the reading. This is the whole
/// new->caps->load->splice->run dance every consumer open-codes, collapsed into one entry: a program that lives
/// on disk as a .slate is invoked by naming it and its inputs — no host-side builder glue, in any language.
/// `caps` is the effect-capability mask (0 = none; the fragment tightens its own grant via compose). Pure: no
/// host I/O — the fragment performs any I/O through the providers its grant allows. The builder is created and
/// freed internally; the returned SlateArray is caller-owned (read via slate_array_*, free with slate_array_free)
/// and outlives this call. Pass `dims` NULL for a scalar reading ({1}). A run that refuses but is well-formed
/// still returns an array whose slate_array_receipt reports refused.
/// @return the reading, or NULL on allocation failure, a malformed fragment, a hole typecheck/arity failure, or
///         a run that produced no array.
SlateArray *slate_invoke(const uint8_t *frag, uint64_t frag_len,
                         const uint8_t *const *args, const uint64_t *arg_lens, uint32_t nargs,
                         uint64_t caps, const int64_t *dims, uint32_t ndims);

/* ---- search: the S axis — one door, measurement (by shape) or value (by computation) ---- */

/// The unified search over the graph rooted at `root`. `operation` selects which of the two searches runs:
///
///   • operation < 0  → measurement (by shape). Find every node reachable from `root` (walking its operands)
///     whose carried shape matches the target {potential P, mode Λ}. A structural relatedness query: it matches
///     on the shape the builder already composed onto each node — not on value, not on node id — so it locates
///     every occurrence of a construction-shape (a `mul` and a `sub` over the same carriers share P but differ
///     in Λ, so both axes are required to match). `potential` is the height bound P (a node's `h_bits`); `mode`
///     is Λ (0 compose, 1 decompose). No value is computed: `out_num`/`out_den`/`out_undef` stay at defaults.
///
///   • operation >= 0 → value (by computation). Run the value search over the collection (a plain op(left,right)
///     composition, elements at the leaves in monotone order). `operation` is a node whose Λ steers, read off
///     the graph — no mode flag: a `sub`-built op (decompose) locates the element equal to `target` (the certified
///     sign of target−element bisects the tree in place, sub-linear); an `add`-built op (compose) aggregates the
///     exact fold of every element (pass `target` = a lit(0) node). `target` names the value sought (a lit node).
///     The settled value — the located element, or the aggregate — is decoded into `*out_num`/`*out_den`; a
///     refused/undefined value sets `*out_undef=1` (with SLATE_BATCH_OK); a value past int64 returns
///     SLATE_BATCH_EWIDE rather than a wrong answer.
///
/// In both modes: matched node ids are written to `out[0 .. min(*out_count, cap))`; `*out_count` receives the
/// TOTAL match count, which may exceed `cap` — re-call with a larger `out` to read them all. `*out_visited`
/// receives the number of vertices walked; `*out_resumable` is 1 if `budget` (0 = unbounded) cut the walk
/// before it finished. Pass NULL for any out you do not want.
/// @return SLATE_BATCH_OK; SLATE_BATCH_EWIDE if a by-value result exceeds int64; SLATE_BATCH_EARGS on a
///         null/poisoned builder or an out-of-range `root` (or, for a by-value query, `target`/`operation`).
/// Target-free `target` sentinels for the extremal measurement: pass one instead of a value node and the
/// decompose descends to the collection's own least / greatest element (min/max), reading it back in
/// `*out_num`/`*out_den`. No supplied value, sized to the collection (sub-linear).
#define SLATE_SEARCH_MIN (-2)
#define SLATE_SEARCH_MAX (-3)
int slate_dag_search(SlateDag *b, int32_t root, uint64_t potential, int32_t mode, uint64_t budget,
                     int32_t *out, uint32_t cap, uint32_t *out_count, uint64_t *out_visited,
                     int32_t *out_resumable, int32_t target, int32_t operation,
                     int64_t *out_num, int64_t *out_den, int32_t *out_undef);

/* ---- result: the receipt (read this — the certification, not the values) ---- */

/// A result's reading: the PSDA certification of the whole dispatch, carried with the result so you hold
/// the certification the instant you hold the result. Read this and keep the values *resident* in the
/// engine (chain another build over the result, or persist with slate_dag_stop) — do not pull cells out.
typedef struct {
  int32_t  exact;    /* D: 1 = Exact reading, 0 = a bracket / no value */
  int32_t  refused;  /* D: 1 = bottom (no authoritative value) — a refused dispatch */
  int32_t  domain;   /* D: the tower set (valued readings): 0 ℕ, 1 ℤ, 2 ℚ, 3 ℝ, 4 ℂ; -1 if refused */
  int32_t  path;     /* S: the tier that computed it — 0 Eager, 1 VM, 2 RNS, 3 Device */
  int32_t  mode;     /* Λ: 0 compose, 1 decompose */
  int32_t  closure;  /* 0 Closed (settled), 4 Incomplete (a budget expired — resumable via stop/restore) */
  uint64_t work;     /* P: the height bound carried into the read */
} slate_reading;

/// Fills `out` with the result's reading (its frame receipt) — no cell values pulled. SLATE_BATCH_OK, or
/// SLATE_BATCH_EARGS on nulls.
int slate_array_receipt(const SlateArray *a, slate_reading *out);

/// The cursor Receipt for a consume `cid` (see slate_dag_consume), valid after slate_dag_run: `out->work` is the
/// next offset (the position, carried in the reading's provenance — not a bare cursor), and `out->closure` is
/// 0 Closed (drained: a short read hit the end) or 4 Incomplete (more to read). Persist this Receipt as the
/// commit; re-present `out->work` as the next consume's offset. Returns SLATE_BATCH_EREFUSED if not yet run.
int slate_dag_cursor(SlateDag *b, uint32_t cid, slate_reading *out);

/// The cell count (∏dims) of a result.
uint64_t slate_array_size(const SlateArray *a);

/* ---- result: extraction (leaving the engine — a last resort, not the normal path) ---- */

/// Unsafe per-cell int64 pull: reconstructs a value per cell into caller `num`/`den` (each >= size),
/// dropping the receipt. This is the line-by-line anti-pattern — prefer keeping the result resident and
/// reading slate_array_receipt, and extract only a final answer, in bulk, via slate_array_records. Kept
/// because a host FFI sometimes needs a raw int lane.
/// @return SLATE_BATCH_OK when every cell fits; SLATE_BATCH_EWIDE (7) if a cell exceeds int64; EREFUSED (4)
///         if a cell is undefined; SLATE_BATCH_EARGS on nulls.
int slate_array_i64_unsafe(const SlateArray *a, int64_t *num, int64_t *den);

/// Deprecated alias for slate_array_i64_unsafe (kept so existing bindings still link). Migrate to the
/// receipt + streaming surface; if you truly need a raw int lane, call the _unsafe name so it reads as one.
int slate_array_i64(const SlateArray *a, int64_t *num, int64_t *den);

/// The wide-bignum bulk readback: one record per cell [valid, sign, L, |num| LE, den LE] (uniform stride
/// 3+2L), the same layout slate_map writes — a single bulk fill, not per-cell. Size-then-fill: on an
/// undersized `out` it sets *out_stride and returns SLATE_BATCH_EOUTSIZE.
int slate_array_records(const SlateArray *a, uint64_t *out, uint64_t out_bytes, uint64_t *out_stride);

/// Frees a result and its cells.
void slate_array_free(SlateArray *a);

#ifdef __cplusplus
}
#endif
