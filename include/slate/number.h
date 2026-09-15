/// @file number.h
/// @brief C ABI for Number — the unified tower value N⊂Z⊂Q⊂R⊂C. The C face of C++ `Slate::Number`: a caller
///        holds a `SlateNum` and combines them, never choosing rational/real/complex — the value rides at the
///        cheapest carrier that holds it exactly and promotes automatically.
///
/// SPDX-License-Identifier: MIT OR Apache-2.0
///
/// Model. A `SlateNumCtx` owns the Arena that real/complex values build their enclosure nodes in; every
/// `SlateNum` is created in a context and must not outlive it. Rational and Gaussian-rational values are
/// exact; real and complex values are certified enclosures read to a precision. Every constructor and
/// operator returns a fresh owned `SlateNum` (NULL on allocation failure); the caller frees each with
/// `slate_num_free`, and the context with `slate_num_ctx_free` once its numbers are gone. Handles passed to
/// one call must all belong to one context.
///
/// Reading. `slate_num_decimal` renders the real-line value (for a complex value, its real part — read the
/// imaginary part via `slate_num_im`) as a certified decimal, exactly like `slate_real_read`. Exactness and
/// the tower domain are queried with `slate_num_is_exact` / `slate_num_domain`.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SlateNumCtx SlateNumCtx;   ///< Owns the Arena real/complex values build in. Single-threaded.
typedef struct SlateNum SlateNum;         ///< An owned tower value (rational / Gaussian / real / complex).

/// Creates a context (owns an Arena) with the default reserve. NULL on allocation failure.
SlateNumCtx *slate_num_ctx_new(void);
/// Creates a context whose Arena reserves `batch_cap_ops` deferred ops before a read is forced (0 = the
/// default reserve). This is the op budget of the context's lazy DAG: a long-lived context that builds many
/// real/complex values accumulates nodes up to this cap before forcing and reclaiming, so a smaller value
/// bounds a context's peak memory (forcing more often), a larger one batches more before a force. Exact
/// ℚ/ℤ/ℂ[i] values build no nodes and are unaffected. Mirrors `slate_dag_new_sized`. NULL on allocation
/// failure.
SlateNumCtx *slate_num_ctx_new_sized(uint64_t batch_cap_ops);
/// Frees a context. Free every `SlateNum` created in it first.
void slate_num_ctx_free(SlateNumCtx *ctx);

/* ---- construction (each returns a fresh owned handle; NULL on failure) ---- */
/// The integer `v` (in ℤ).
SlateNum *slate_num_int(SlateNumCtx *ctx, int64_t v);
/// The rational `num`/`den` (in ℚ). A zero denominator yields the PSDA bottom (undefined), never a crash.
SlateNum *slate_num_rat(SlateNumCtx *ctx, int64_t num, int64_t den);
/// π on the real line.
SlateNum *slate_num_pi(SlateNumCtx *ctx);
/// e = exp(1) on the real line.
SlateNum *slate_num_e(SlateNumCtx *ctx);
/// The imaginary unit i — an exact Gaussian rational (0 + 1·i).
SlateNum *slate_num_i(SlateNumCtx *ctx);
/// A copy of `a` (a fresh owned handle over the same value).
SlateNum *slate_num_copy(const SlateNum *a);
/// Frees a number handle.
void slate_num_free(SlateNum *a);

/* ---- arithmetic (a and b must share a context) ---- */
SlateNum *slate_num_add(const SlateNum *a, const SlateNum *b);
SlateNum *slate_num_sub(const SlateNum *a, const SlateNum *b);
SlateNum *slate_num_mul(const SlateNum *a, const SlateNum *b);
SlateNum *slate_num_div(const SlateNum *a, const SlateNum *b);
SlateNum *slate_num_neg(const SlateNum *a);

/* ---- irrational (real-line) operations — promote to ℝ ---- */
SlateNum *slate_num_sqrt(const SlateNum *a);
SlateNum *slate_num_exp(const SlateNum *a);
SlateNum *slate_num_ln(const SlateNum *a);
SlateNum *slate_num_atan(const SlateNum *a);
SlateNum *slate_num_sin(const SlateNum *a);
SlateNum *slate_num_cos(const SlateNum *a);

/* ---- complex structure ---- */
/// The real part (the value itself when real).
SlateNum *slate_num_re(const SlateNum *a);
/// The imaginary part (0 when real).
SlateNum *slate_num_im(const SlateNum *a);
/// The complex conjugate (a no-op on the real line).
SlateNum *slate_num_conj(const SlateNum *a);
/// The modulus |z| (exact on the rational path, else a certified real √(re²+im²)).
SlateNum *slate_num_abs(const SlateNum *a);
/// The value re-homed to the lowest carrier its reading certifies — free where the chain already proved it,
/// refused (unchanged) where the demotion needs an undecidable decision the budget cannot settle.
SlateNum *slate_num_canonical(const SlateNum *a);

/* ---- queries ---- */
/// The runtime tower domain: 0=ℕ, 1=ℤ, 2=ℚ, 3=ℝ, 4=ℂ (−1 on a null handle).
int slate_num_domain(const SlateNum *a);
/// 1 if the value is exact (a point / Gaussian rational), 0 if a proper enclosure (or null).
int slate_num_is_exact(const SlateNum *a);
/// 1 if the value is the PSDA bottom (a domain edge: ÷0, sqrt of a negative, ln of a non-positive), else 0.
int slate_num_is_undefined(const SlateNum *a);
/// The sign of the real-line value: −1, 0, or +1. If `out_decided` is non-NULL it receives 1 when decided,
/// 0 when a real enclosure could not exclude 0 within the budget (never a guess).
int slate_num_sign(const SlateNum *a, int32_t *out_decided);

/// Renders the real-line value (a complex value's real part) as a certified decimal string, MSD-first, into
/// `out` (not NUL-terminated), refining a real enclosure to `prec_bits` and capping fractional digits at
/// `max_digits`. `out_certified` (if non-NULL) receives the certified fractional-digit count (SIZE_MAX for an
/// exact value); `out_exact` (if non-NULL) receives 1 iff the value read Exact. Returns the string length
/// (≥ 0) or a negative code: −1 bad args, −4 `out` too small, −100 an error at the C boundary.
int slate_num_decimal(const SlateNum *a, uint32_t prec_bits, int32_t max_digits, char *out, uint64_t out_len,
                      uint64_t *out_certified, int32_t *out_exact);

/// The exact fraction of an exact rational value (ℕ/ℤ/ℚ), as little-endian magnitude limbs with the sign
/// (0 nonnegative, 1 negative) on the numerator. Refuses (SLATE_BATCH_EREFUSED) for an inexact real enclosure,
/// a complex value, or a bottom — use `slate_num_decimal`, or `slate_num_re`/`slate_num_im`, there. Size-then-
/// fill: pass `num`/`den` NULL to learn the limb counts (`*nnum`/`*nden`) and the sign, then call again with
/// buffers of at least that many `uint64_t`s (SLATE_BATCH_EOUTSIZE if short, with `*nnum`/`*nden` set to need).
int slate_num_fraction(const SlateNum *a, uint32_t prec_bits, int32_t *sign,
                       uint64_t *num, uint64_t *nnum, uint64_t *den, uint64_t *nden);

/* ---- the grid bridge: read a dispatched array result as tower values ---- */
typedef struct SlateArray SlateArray;   ///< A grid result from slate_dag_run (defined by slate/array.h).
/// Reads cell `i` of a dispatched grid result as a Number in `ctx` — the bulk tower bridge. The grid engine is
/// the exact integer/rational tier, so an exact cell becomes a rational at its true rung (ℕ/ℤ/ℚ by value); a
/// refused or out-of-range cell becomes the PSDA bottom (undefined). Returns a fresh owned handle (NULL on
/// bad args / allocation failure). `arr` and `ctx` are independent — the cell's value is read into `ctx`.
SlateNum *slate_num_from_array(SlateNumCtx *ctx, const SlateArray *arr, uint64_t i);

#ifdef __cplusplus
}
#endif
