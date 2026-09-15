/// @file number_grid.cpp
/// @brief Gate: a dispatched grid reads back as Number values — the bulk tower bridge. The grid engine is the
/// exact integer/rational tier, so `run()` cells come back as Numbers at their true rung (ℕ/ℤ for integers, ℚ
/// for fractions), unifying the grid output with the scalar Number surface.
///
/// SPDX-License-Identifier: MIT OR Apache-2.0

#include "slate/number_grid.hpp"

#include <cstdio>
#include <memory>
#include <vector>

using namespace Slate;

static long fails = 0;
static void check(bool ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }

static bool eq_q(const Receipt &r, int sign, uint64_t num, uint64_t den) {
  if (!r.is_exact()) return false;
  uint64_t n = r.num.empty() ? 0 : r.num[0], d = r.den.empty() ? 1 : r.den[0];
  return r.sign == sign && n == num && d == den;
}

static std::shared_ptr<const WBuffer> ivec(std::vector<int64_t> v) {
  auto b = std::make_shared<WBuffer>(v.size(), 1);
  for (size_t i = 0; i < v.size(); i++) b->set(i, v[i]);
  return std::static_pointer_cast<const WBuffer>(b);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  Arena a(((size_t)1) << 20);

  /* 1) an integer map grid: z[i] = x[i]*2 + 1 over x = [0,1,2,3], dispatched over {4}. Every cell reads back
   *    as a Number in ℕ (a nonnegative integer), value-correct, from the same grid the engine vectorised. */
  {
    DagBuild b(a);
    unsigned cX = b.carrier(ivec({0, 1, 2, 3}));
    int32_t i = b.param(0);
    int32_t root = b.add(b.mul(b.load(cX, i), b.lit(2)), b.lit(1));   /* 2x + 1 */
    ArrayReading R = b.run(root, {4});
    std::vector<Number> nums = cell_numbers(a, R);
    check(nums.size() == 4, "grid has 4 Number cells");
    const int64_t want[] = {1, 3, 5, 7};
    for (int k = 0; k < 4; k++) {
      check(!nums[(size_t)k].is_complex() && nums[(size_t)k].domain() == Domain::N,
            "integer grid cell is a Number in N");
      check(eq_q(nums[(size_t)k].read(), 0, (uint64_t)want[k], 1), "grid cell reads its exact integer value");
    }
    /* a Number cell composes with the scalar surface: cell(2) + 1/2 promotes to ℚ, exact 11/2. */
    Number mixed = cell_number(a, R, 2) + Number(a, 1, 2);   /* 5 + 1/2 */
    check(mixed.domain() == Domain::Q && eq_q(mixed.read(), 0, 11, 2), "a grid Number composes with a rational: 5 + 1/2 = 11/2");
  }

  /* 2) a rational grid: cells that carry a denominator read back as Numbers in ℚ. Here a signed matmul of
   *    rational operands (num/den), so a cell like 58/15 comes back domain ℚ, exact. */
  {
    const int hbits = 8 + 6 + 8;
    DagBuild b(a);
    auto A = std::make_shared<WBuffer>(WBuffer::rns_rational({1, 2, 3, 4, 5, 6}, 3, hbits));    /* A = An/3, 2x3 */
    auto B = std::make_shared<WBuffer>(WBuffer::rns_rational({7, 8, 9, 10, 11, 12}, 5, hbits));  /* B = Bn/5, 3x2 */
    unsigned cA = b.carrier(A), cB = b.carrier(B);
    int32_t i = b.param(0), j = b.param(1);
    int32_t root = -1;                                          /* Σ_l A[i·3+l]·B[l·2+j], built from primitives */
    for (int l = 0; l < 3; l++) {
      int32_t ll = b.lit(l);
      int32_t idxA = b.add(b.mul(i, b.lit(3)), ll);
      int32_t idxB = b.add(b.mul(ll, b.lit(2)), j);
      int32_t prod = b.mul(b.load(cA, idxA), b.load(cB, idxB));
      root = (root < 0) ? prod : b.add(root, prod);
    }
    Potential g(hbits);                                        /* declare the result height so cells reconstruct */
    ArrayReading R = b.run(root, {2, 2});
    std::vector<Number> nums = cell_numbers(a, R);
    check(nums.size() == 4, "rational grid has 4 Number cells");
    /* C = [58,64,139,154]/15 (already lowest terms). */
    const uint64_t wn[] = {58, 64, 139, 154};
    for (int k = 0; k < 4; k++) {
      check(nums[(size_t)k].domain() == Domain::Q && nums[(size_t)k].is_exact(), "rational grid cell is exact ℚ");
      check(eq_q(nums[(size_t)k].read(), 0, wn[k], 15), "rational grid cell reads its exact num/15");
    }
  }

  printf("%s number_grid: a dispatched grid reads back as Number values — integer cells in ℕ, rational cells "
         "in ℚ, each exact and value-correct, and a grid Number composes with the scalar Number surface\n",
         fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
