/// @file number_grid.hpp
/// @brief Read a dispatched grid (`ArrayReading`) as tower values — the bulk `Number` bridge.
///
/// SPDX-License-Identifier: MIT OR Apache-2.0
///
/// `DagBuild::run(root, dims)` dispatches a construction over a product grid and returns an `ArrayReading`:
/// ∏dims exact cells with a provenance frame. The grid engine is the exact integer/rational tier, so every
/// cell is an exact rational at its true tower rung (ℕ/ℤ for an integer cell, ℚ for a fractional one). This
/// header reads those cells back as `Number`, so a grid result and the scalar `Number` surface share one
/// vocabulary. (Per-cell irrationals are the scalar `node_eval` path, not a grid operation.)
///
/// A bridge kept separate from `build.hpp`: the DAG builder stays the minimal exact substrate and never
/// depends on the reals/complex surface; only a consumer that wants tower values pulls this in.
#pragma once

#include <vector>
#include "slate/dag/build.hpp"
#include "slate/number.hpp"

namespace Slate {

/// Cell `i` of a grid result as a `Number`. An exact cell becomes an exact rational (its runtime domain ℕ/ℤ/ℚ
/// read off the value); a refused / out-of-range cell becomes the PSDA bottom (undefined). The rational nodes
/// are built in `cx`.
inline Number cell_number(Arena &cx, const ArrayReading &ar, size_t i) {
  Receipt r = ar.cell(i);
  if (!r.has_value() || !r.is_exact()) return Number(Rational::undefined(cx));
  return Number(Rational::from_reading(cx, r.sign, r.num, r.den));
}

/// The whole grid as `Number` values, in row-major cell order (∏dims of them).
inline std::vector<Number> cell_numbers(Arena &cx, const ArrayReading &ar) {
  std::vector<Number> out;
  out.reserve(ar.size());
  for (size_t i = 0; i < ar.size(); i++) out.push_back(cell_number(cx, ar, i));
  return out;
}

}  // namespace Slate
