/// @file reals.h
/// @brief C ABI for the certified-reals front door: a real-dialect program → N certified decimal digits.
///
/// SPDX-License-Identifier: MIT OR Apache-2.0
///
/// The batch door (batch.h) computes exact rationals. This door computes *irrationals* — √, exp, ln,
/// sin/cos/atan, π, e and any composition — to a caller-chosen precision, returning a certified
/// decimal enclosure (only digits both endpoints of the proven bracket agree on are emitted). It is
/// the C face of the header-only C++ `Slate::Real`; the same certified-interval evaluator underneath.
///
/// Real-dialect program (a stack machine, opcode in the high 16 bits, immediate in the low 16 —
/// the same shape as the fold dialect):
///     0  PUSH imm     push the small non-negative integer `imm` (0..65535) as a real constant
///     1  ADD          pop b, a; push a + b
///     2  MUL          pop b, a; push a * b
///     3  SUB          pop b, a; push a - b
///     9  DIV          pop b, a; push a / b   (so 1/2 = PUSH 1, PUSH 2, DIV)
///     20 SQRT         pop x; push sqrt(x)
///     21 EXP          pop x; push exp(x)
///     22 LN           pop x; push ln(x)
///     23 ATAN         pop x; push atan(x)
///     24 SIN          pop x; push sin(x)
///     25 COS          pop x; push cos(x)
///     26 PI           push pi
///     27 E            push e
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Evaluates a real-dialect program to `prec_bits` and writes up to `max_digits` certified fractional
/// decimal digits of the result into `out` (MSD-first, e.g. "1.414213562...", not NUL-terminated).
///
/// @param prog       Real-dialect program words; caller owns.
/// @param plen       Program length in words.
/// @param prec_bits  Target bracket precision: refine until the enclosure width <= 2^-prec_bits.
/// @param max_digits Cap on fractional digits rendered (the integer part is always emitted in full).
/// @param out        Buffer for the decimal string; caller owns and frees it.
/// @param out_len    Size of `out` in chars. A rendering that would exceed it is refused (returns -4).
/// @param out_certified If non-NULL, receives the count of certified fractional digits (SIZE_MAX for
///                   an exact value; a shared-prefix count for an irrational bracket).
/// @param out_exact  If non-NULL, receives 1 if the value read back Exact (rational), else 0 (a bracket).
/// @return The decimal string length in chars (>= 0), or a negative code: -1 bad args / empty program,
///         -2 unknown opcode, -3 the program did not leave exactly one value on the stack (or a stack
///         underflow), -4 `out` too small, -100 an error stopped at the C boundary.
int slate_real_read(const uint32_t *prog, uint64_t plen, uint32_t prec_bits, int32_t max_digits,
                    char *out, uint64_t out_len, uint64_t *out_certified, int32_t *out_exact);

#ifdef __cplusplus
}
#endif
