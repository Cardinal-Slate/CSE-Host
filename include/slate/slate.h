/// @file slate.h
/// @brief The C ABI, user band: build a record from binaries, run it, read the reading. Everything travels one
///        arc — record → word → store → lane → reading — and this header is the whole of that arc from C.
///
/// SPDX-License-Identifier: MIT OR Apache-2.0
///
/// `Slate::DagBuild` (dag/build.hpp) with each node handle carried across the C boundary as the int it already
/// is. Only node ids cross the ABI. A builder holds an Arena; a binary registers as a carrier; a record is composed
/// from leaves and the six ops; `run` dispatches the record over a product grid and returns a result owning the
/// grid of cells and its receipt. A binary is a number (its bytes are the limbs of an integer), and the relation
/// between two binaries is one rational in ℚ — `div` — whose reading is the reduced fraction; every global
/// question about the pair (order, divides, the quotient's height) is a reading of that relation, never a
/// recomposition. The store rides the region's codec (`slate_dag_codec`): every reading is looked up by its word
/// before a lane runs and put after, so a second identical construction anywhere is a hit.
///
/// Two more bands sit beside this one: `slate/embed.h` (an embedder's effects, sandbox, async, search) and
/// `slate/tune.h` (cost knobs — who computes, never the value). A provider links the floor headers (`rns.h`,
/// `device/seam.h`, `gpu.h`, `profile.h`).
///
/// Ownership & threading: a builder and its results are caller-owned (free with slate_dag_free /
/// slate_array_free). An Arena is single-threaded — a SlateDag must not be shared across threads; a SlateArray,
/// once produced, is an immutable owned buffer readable from any thread.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "slate/trace.h"    /* slate_entry — what a receipt says */

/* A call answers NULL when it did what was asked, else the name of the refusal — a static string, one of:
 *   "args"       a null builder/pointer, an id out of range, an empty program, a name that is not one listed
 *   "outsize"    the caller's buffer is smaller than the record needs — size it first (a size query)
 *   "refused"    the reading has no value (a cell undefined, a hole that failed its receipt) — never a partial set
 *   "wide"       (the int64 doors) the value is exact but exceeds a signed 64-bit rational — not a refusal: read
 *                the lossless records instead
 *   "nomem"      the engine could not allocate its working memory
 *   "internal"   any other error stopped at the C boundary
 * A refusal means nothing was written, so `rc != NULL` is the complete failure check — never an abort, never a
 * silent wrong value. */
/// Whether `rc` (a call's answer) is the refusal named `name`.
static inline int slate_refused(const char *rc, const char *name) {
  if (!rc || !name) return 0;
  while (*rc && *rc == *name) { rc++; name++; }
  return *rc == *name;
}

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

/* ---- the region: the a-priori height and the store ---- */

/// Declares the Potential — the a-priori height ceiling in bits — so a carried (rational) result provisions
/// enough channels for the whole computation's height. Auto-derived from the record (its height walk) and from
/// rational carriers when left unset; the larger applies. A knob on who computes to what height, never the value.
/// Returns NULL, or "args" on a null builder.
const char *slate_dag_potential(SlateDag *b, uint32_t bits);

/// Install the codec and store on this builder's region: `encode` (bytes → the word), `decode` (the word → the
/// bytes under it, or nonzero = not there) and `put` ((word, bytes) out), each with the perform contract (malloc'd
/// *out/*outn, return 0), and `secret` — the shared secret, `sn` bytes, copied — handed to all three. Pass null
/// for a callback not offered: encode defaults to a generic hash of the bytes keyed by the secret. The store is
/// the only memory: everything the region computes — readings, compiled programs, effects, residue promotions,
/// the relations the lane keeps — is looked up by its word before it runs and put after. With no decode
/// installed nothing is kept and every ask runs. The region's tasks inherit it.
const char *slate_dag_codec(SlateDag *b,
    int (*encode)(const uint8_t *bytes, uint64_t n, const uint8_t *secret, uint64_t sn, uint8_t **out, uint64_t *outn, void *user),
    int (*decode)(const uint8_t *word, uint64_t wn, const uint8_t *secret, uint64_t sn, uint8_t **out, uint64_t *outn, void *user),
    int (*put)(const uint8_t *word, uint64_t wn, const uint8_t *bytes, uint64_t n, const uint8_t *secret, uint64_t sn, void *user),
    const uint8_t *secret, uint64_t sn, void *user);

/* ---- leaves: the binaries and the grid ---- */

/// A binary crosses the boundary: `n` raw bytes registered as a carrier of byte cells (min width, one bulk copy).
/// Read cell i with slate_dag_load; a Horner fold over the cells is the binary as a number. Returns the carrier
/// id, or UINT32_MAX on bad args (null builder/bytes with n>0).
uint32_t slate_dag_carrier_bytes(SlateDag *b, const uint8_t *bytes, uint64_t n);

/// Registers an array of `n` int64 values, copied into the builder. Returns its carrier id (for slate_dag_load),
/// or UINT32_MAX on bad args (null builder/vals with n>0).
uint32_t slate_dag_carrier(SlateDag *b, const int64_t *vals, uint64_t n);

/// Registers a rational array cleared to a common denominator: cell i = `nums[i]` / `den` (one shared
/// denominator), folded into residues so a reduce stays an integer channel-dot and the readback recovers each
/// cell's num/den. `hbits` is the whole computation's result height (numerator + denominator bits, with margin)
/// — provision it to the result, not the operand, so an under-provisioned cell decodes to undefined, never a
/// wrong value. All rational carriers in one build should pass the same `hbits`; slate_dag_run scopes its
/// Potential to the max. Returns its carrier id, or UINT32_MAX on bad args (`den` == 0, negative `hbits`, null
/// builder/nums). Mixing rational and plain int64 carriers in one build is not supported.
uint32_t slate_dag_carrier_q(SlateDag *b, const int64_t *nums, uint64_t n, int64_t den, int32_t hbits);

/// Swap carrier `cid`'s bytes in place, then re-run the same construction over the new source — no rebuild, no
/// recompile (the compiled leaf is carrier-independent). Returns NULL or "args".
const char *slate_dag_carrier_set_bytes(SlateDag *b, uint32_t cid, const uint8_t *bytes, uint64_t n);
/// Swap carrier `cid`'s int64 values in place (the same U/E split for an int64 carrier). Returns NULL or "args".
const char *slate_dag_carrier_set(SlateDag *b, uint32_t cid, const int64_t *vals, uint64_t n);

/// carrier[cid] read at node `idx` (any index expression: a lit, a param, or arithmetic over them).
int32_t slate_dag_load(SlateDag *b, uint32_t cid, int32_t idx);

/// A grid coordinate: the value of dispatch axis `slot` (0-based into the `dims` passed to run) at each cell.
int32_t slate_dag_param(SlateDag *b, uint32_t slot);

/// An integer constant leaf.
int32_t slate_dag_lit(SlateDag *b, int64_t v);

/* ---- ops: the record's composition ---- */

int32_t slate_dag_add(SlateDag *b, int32_t x, int32_t y);
int32_t slate_dag_mul(SlateDag *b, int32_t x, int32_t y);
int32_t slate_dag_sub(SlateDag *b, int32_t x, int32_t y);
/// The relation x/y in ℚ: the reading leaves ℤ for ℚ (the receipt's domain says so). Runs on the residue lane,
/// exactly, sign a channel, and reads back as a reduced fraction (slate_array_records; slate_array_i64_unsafe's
/// num/den). A cell whose divisor is zero refuses the run — never a value.
int32_t slate_dag_div(SlateDag *b, int32_t x, int32_t y);
/// Signed high-multiply (smulh): the high 64 bits of x*y. The decompose-family basis (floor-div,
/// reduce-to-canonical) is built from mulh + asr + sub, so a fragment library can express it.
int32_t slate_dag_mulh(SlateDag *b, int32_t x, int32_t y);
/// Arithmetic shift right of `x` by `sh` (an integer-constant leaf, i.e. from slate_dag_lit).
int32_t slate_dag_asr(SlateDag *b, int32_t x, int32_t sh);

/// Reflection: read one node's structure off the graph. A fragment spliced into a builder is a graph of these —
/// walking ids 0..root recovers its exact construction (op + child ids + leaf value). `*kind` is the node's kind
/// by name: "op", "param", "lit", "carrier" or "big". For an op, `*op` is its name ("add", "sub", "mul", "div",
/// "mulh", "asr") and `*a`/`*bchild` are child node ids (-1 if absent), else `*op` is NULL; for a lit,
/// `*lit_n`/`*lit_d` are the value. Pass NULL for any out you do not want.
/// @return NULL; "args" on a null builder or an out-of-range/invalid `id`.
const char *slate_dag_node(SlateDag *b, int32_t id, const char **kind, const char **op,
                   int32_t *a, int32_t *bchild, int64_t *lit_n, int64_t *lit_d);

/* ---- run ---- */

/// Dispatches the record rooted at `root` over the product grid `dims` (ndims axes), returning a result of
/// ∏dims cells. Pass dims = {1} for one scalar reading. Returns NULL on a refused dispatch: a malformed builder
/// (a bad node/carrier id was passed earlier) or a non-lowerable leaf — never a wrong value. The builder may be
/// freed or reused (build a new root) after this call.
SlateArray *slate_dag_run(SlateDag *b, int32_t root, const int64_t *dims, uint32_t ndims);

/* ---- result: the receipt (read this — the certification, not the values) ---- */

/// A result's reading: the PSDA certification of the whole dispatch, carried with the result so you hold
/// the certification the instant you hold the result — as entries, each what the engine holds, by name:
///   verdict   utf8      exact | bracket | undefined   (D; undefined = a refused dispatch, no authoritative value)
///   domain    utf8      N | Z | Q | R | C             (the tower set of a valued reading; absent when refused)
///   path      utf8      eager | vm | rns | device | effect   (S — the tier that computed it)
///   mode      utf8      compose | decompose           (Λ)
///   closure   utf8      closed | incomplete | invalid (incomplete: a budget expired — resumable via stop/restore)
///   evidence  utf8      exact | bracket | empirical | none
///   outcome   utf8      value | not-defined | no-authoritative-value
///   height    bits:u64  (P — the height bound carried into the read)
/// The entries live as long as `a`. Read this and keep the values *resident* in the engine (chain another
/// build over the result) — do not pull cells out. NULL, or "args" on nulls.
const char *slate_array_receipt(const SlateArray *a, const slate_entry **entries, int32_t *n);

/// The cell count (∏dims) of a result.
uint64_t slate_array_size(const SlateArray *a);

/// The lossless bulk readback: one record per cell [valid, sign, L, |num| LE, den LE] (uniform stride 3+2L u64
/// words) — a single bulk fill, not per-cell. Size-then-fill: on an undersized (or NULL) `out` it sets
/// *out_stride and returns "outsize".
const char *slate_array_records(const SlateArray *a, uint64_t *out, uint64_t out_bytes, uint64_t *out_stride);

/// Unsafe per-cell int64 pull: reconstructs a value per cell into caller `num`/`den` (each >= size), dropping
/// the receipt. Kept because a host FFI sometimes needs a raw int lane; prefer the receipt and, for a final
/// answer, slate_array_records.
/// @return NULL when every cell fits; "wide" if a cell exceeds int64; "refused" if a cell is undefined;
///         "args" on nulls.
const char *slate_array_i64_unsafe(const SlateArray *a, int64_t *num, int64_t *den);

/// Frees a result and its cells.
void slate_array_free(SlateArray *a);

/* ---- fragment libraries: a serialized record with typed holes, kept as a leaf -----------------------------
 *
 * A fragment is a construction with unbound carriers (holes) — a reusable graph piece, not a finished
 * computation. save_fragment records the sub-DAG rooted at a node as normalized builder instructions (the
 * portable source form, never lowered nodes — each host re-lowers in its own context) and keeps those bytes as
 * a leaf: one cell per byte, under their own word, through the same door as any bytes handed in. load reads
 * that leaf back by word and byte count and parses it; splice replays it into another builder, wiring the
 * caller's carriers into its holes and returning the new root node id. Receipts compose at the seam. Any
 * container whose store holds the leaf loads and splices it, so a construction travels as a name and never as
 * bytes; evaluation is referentially transparent over canonical ℚ, so a spliced fragment's reading is a
 * pure function of the graph — keep, load, run reproduces it wherever the word resolves, or refuses.
 *
 * A program's name, wherever one is carried in a single byte string (slate_dag_program, the "slate.emit"
 * answer, the "slate.run" request, the "slate.tail" hand-off), is `word ‖ count`: the word's bytes, then the
 * byte count as 8 little-endian bytes. The doors here take the two as separate arguments. */

/// A fragment interface receipt — the *signature* of a hole or the root (what it expects / produces).
typedef struct {
  const char *kind;    /* "scalar" (a per-cell value: the root) or "array" (a carrier: a hole) */
  const char *domain;  /* the expected tower set: "N", "Z", "Q", "R" or "C"; NULL = derive / any */
  const char *mode;    /* Λ: "compose" or "decompose"; NULL = any */
  uint64_t    h_bits;  /* the Potential (height in bits) the fragment provisions for; 0 = derive */
} slate_iface_receipt;

/// One unbound carrier (a hole the caller must fill at splice time).
typedef struct {
  uint32_t slot;                /* the hole's index — pass its carrier at arg_carriers[slot] in slate_dag_splice */
  slate_iface_receipt receipt;  /* what the caller must supply */
} slate_hole;

/// Keep the sub-DAG rooted at `root` as a fragment leaf, declaring the carrier ids in `holes` (nholes of them)
/// as unbound inputs, and hand back its name: `*word` (a malloc'd buffer of `*wn` bytes — the caller frees it
/// with free()) and `*pn`, the leaf's byte count. Carriers referenced by `root` but not listed in `holes` are
/// baked in as constants (their int64 cell values are serialized) — a baked carrier must be a plain positional
/// integer array (an RNS/float carrier must be a hole). The bytes go through the region's store (slate_dag_codec)
/// as one cell per byte; a leaf already kept under that word is a hit and is not put again. Nothing is written
/// anywhere else: there is no sink and no file.
/// @return NULL; "args" (null/poisoned builder, a null out, bad root, a hole id that is not a carrier `root`
///         reads, or a non-serializable baked carrier); "refused" (the store kept nothing — a program nobody
///         can read is not a program); "nomem"; "internal".
const char *slate_dag_save_fragment(SlateDag *b, int32_t root, const uint32_t *holes, uint32_t nholes,
                            uint8_t **word, uint64_t *wn, uint64_t *pn);

/// Read the fragment leaf `word`/`wn` names — `pn` bytes of it — out of `b`'s store and parse it. Fully
/// validated before it is usable (back-reference-only child ids so the graph is acyclic, in-range
/// carrier/param references, bounded sizes, a crc over the body, and a length that is exactly the leaf's), so
/// a malformed or hostile leaf returns NULL rather than a bad fragment. A word the store does not hold — or, on
/// a machine carrying one share of a roster, a leaf the door in front of the store could not gather whole — is
/// a miss and returns NULL, never a guess. Caller owns; free with slate_frag_free. The fragment is immutable
/// and may be spliced into many builders and read from any thread.
SlateFrag *slate_frag_load(SlateDag *b, const uint8_t *word, uint64_t wn, uint64_t pn);

/// Report the interface: the number of grid params, the holes and their receipts, and the root receipt. Pass
/// NULL for any out you do not want. Size-then-fill for the holes: pass `holes_out` NULL to read the count in
/// `*nholes_io`, then again with a buffer of that many entries.
/// @return NULL; "args" on a null fragment; "outsize" if `holes_out` is non-NULL and `*nholes_io` is
///         smaller than the hole count (`*nholes_io` is set to the needed count).
const char *slate_frag_iface(const SlateFrag *f, uint32_t *nparams, slate_hole *holes_out, uint32_t *nholes_io,
                     slate_iface_receipt *root_out);

/// Splice fragment `f` into builder `b`: replay its instructions (remapping its node ids into b's id space
/// and composing receipts at the seam), registering its baked carriers into b and wiring the caller's
/// carriers into its holes. `arg_carriers[i]` is the carrier id (already registered in b) for hole i, in the
/// order slate_frag_iface reports; `narg` must equal the hole count. A hole whose supplied carrier's domain
/// is incompatible with the declared receipt refuses before any node is emitted, so b is left unmodified.
/// @return NULL with `*out_root` the new root node id in b; else the refusal — "args" (null/arity/id),
///         "refused" (a hole failed its receipt typecheck), "nomem", "internal" — with `*out_root` -1.
const char *slate_dag_splice(SlateDag *b, const SlateFrag *f, const uint32_t *arg_carriers, uint32_t narg,
                             int32_t *out_root);

/// Free a loaded fragment. Node results and builders already produced from a splice stay valid (splice copies).
void slate_frag_free(SlateFrag *f);

/// The executable invariant, as one call. Read the fragment leaf `word`/`wn`/`pn` names out of `b`'s store,
/// register each byte-arg as a carrier and wire them into the fragment's holes (in slate_frag_iface order;
/// `nargs` must equal the hole count), run the spliced root over `dims`/`ndims`, and return the reading: a
/// program that lives in the store is invoked by naming it and its inputs — no host-side builder glue, in any
/// language. `caps` are the `ncaps` effect class names granted (0 = none; see slate/embed.h). Pure: no host
/// I/O — the fragment performs any I/O through the providers its grant allows. The container is the caller's,
/// because the store is: install it with slate_dag_codec and free the builder when done. The returned
/// SlateArray is caller-owned (read via slate_array_*, free with slate_array_free) and outlives the builder.
/// Pass `dims` NULL for a scalar reading ({1}). A run that refuses but is well-formed still returns an array
/// whose slate_array_receipt reports refused.
/// @return the reading, or NULL on a null builder, a word the store does not hold, a malformed fragment, a hole
///         typecheck/arity failure, or a run that produced no array.
SlateArray *slate_invoke(SlateDag *b, const uint8_t *word, uint64_t wn, uint64_t pn,
                         const uint8_t *const *args, const uint64_t *arg_lens, uint32_t nargs,
                         const char *const *caps, uint32_t ncaps, const int64_t *dims, uint32_t ndims);

#ifdef __cplusplus
}
#endif
