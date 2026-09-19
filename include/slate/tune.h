/// @file tune.h
/// @brief The C ABI, cost band: the knobs on a builder's region. Each chooses who computes, how many, or to
///        what a-priori height — never the value, so a reading is exact under any setting. Extends
///        slate/slate.h; a user who accepts the defaults never needs it.
///
/// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "slate/slate.h"
#include "slate/trace.h"    /* slate_emit — the telemetry sink */

#ifdef __cplusplus
extern "C" {
#endif

/* Each setter patches the builder's region (the same Envelope the C++ Region guards patch); slate_dag_run
 * activates it for the dispatch and restores it after. Setters compose (last write per axis wins) and return
 * NULL, or "args" on a null builder / out-of-range argument. */

/// Caps host fan-out at `n` threads (0 = hardware concurrency). A cost knob: it changes timing, not the result.
const char *slate_dag_threads(SlateDag *b, uint32_t n);

/// Routes the dispatch to the device (GPU) executor when a backend is linked. A no-op with no backend, and the
/// driver sample-checks the device result and falls back to the host on any mismatch, so it never changes a value.
const char *slate_dag_device(SlateDag *b, int on);

/// Forces the leaf JIT on (1) or off (0). Off runs the interpreter — for an A/B baseline against the compiled lane.
const char *slate_dag_jit(SlateDag *b, int on);

/// Forces leaf vectorisation on (1) or off (0). Off keeps the per-item lane, the same result more slowly.
const char *slate_dag_vec(SlateDag *b, int on);

/// Sets the telemetry level by name: "off", "timing" or "profile". Pair with slate_dag_trace to install a
/// destination; with no sink the engine gates the emit sites off and pays nothing. "args" on a name that is
/// not a level.
const char *slate_dag_telemetry(SlateDag *b, const char *level);

/// Installs the telemetry sink `emit` (with `user`) that receipts stream to; NULL clears it. The builder holds
/// the sink for its lifetime, so it must outlive the runs that emit through it.
const char *slate_dag_trace(SlateDag *b, slate_emit emit, void *user);

/// Sets the real-read bracket width target (2^-prec_bits) — how tightly a real value is resolved.
const char *slate_dag_width(SlateDag *b, uint32_t prec_bits);

/// Caps a real read's refine iterations (0 = the loop's own cap). Bounds work on a value that refines forever.
const char *slate_dag_work(SlateDag *b, uint64_t cap);

/// Caps per-read scratch at `bytes`; a real read that would exceed it refuses rather than exhausting memory.
const char *slate_dag_mem(SlateDag *b, uint64_t bytes);

/// Constrains RNS channel selection, by lane name ("auto", "i16", "i32", "i64", "f32", "f64"): `pin` forces one
/// lane (NULL or "auto" = the cost model chooses); `allow` lists the `nallow` permitted lanes (NULL = every lane).
/// A cost/routing knob over the residue lanes, never the value. "args" on a name that is not a lane.
const char *slate_dag_channel(SlateDag *b, const char *pin, const char *const *allow, uint32_t nallow);

/// Sets the RNS channel cost model for one lane — the coefficients the planner ranks that lane by: `gmacs` its
/// throughput (giga-MACs/sec) and `dispatch_ms` its fixed per-channel dispatch cost (ms); a negative value leaves
/// that coefficient unchanged. `lane` is a lane name as for slate_dag_channel. Calibration for cross-hardware
/// benchmarking: it moves which lane the planner picks, never the value.
const char *slate_dag_channel_cost(SlateDag *b, const char *lane, double gmacs, double dispatch_ms);

/// Sets the operand-decompose cost per element for narrow (≤ int64) and wide (bignum) operands (ms); a negative
/// value leaves that coefficient unchanged.
const char *slate_dag_decomp_cost(SlateDag *b, double narrow, double wide);

#ifdef __cplusplus
}
#endif
