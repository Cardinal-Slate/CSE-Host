/* The Number tower from pure C. No C++ in this translation unit: includes slate/number.h only, builds
 * tower values through the handle ABI, and checks that ℚ stays exact, an irrational op promotes to ℝ, the
 * imaginary unit gives exact Gaussian arithmetic, and the reading/domain/canonical queries agree with the
 * C++ Number — the FFI caller never chooses between rational, real, and complex. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "slate/number.h"
#include "slate/array.h"

static int fails = 0;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); ++fails; } }

/* render the real-line value to a decimal string (NUL-terminated); return exactness via *exact. */
static const char *dec(const SlateNum *n, char *buf, size_t cap, int *exact) {
  int32_t ex = 0;
  int len = slate_num_decimal(n, 256, 32, buf, cap - 1, NULL, &ex);
  if (len < 0) { snprintf(buf, cap, "<err %d>", len); if (exact) *exact = 0; return buf; }
  buf[len] = '\0';
  if (exact) *exact = ex;
  return buf;
}

/* Domain codes: 0=N 1=Z 2=Q 3=R 4=C */
enum { DOM_N = 0, DOM_Z = 1, DOM_Q = 2, DOM_R = 3, DOM_C = 4 };

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  SlateNumCtx *cx = slate_num_ctx_new();
  if (!cx) { printf("FAIL: ctx\n"); return 1; }
  char buf[256];
  int exact;

  /* 1) ℚ stays exact: 3 + 1/2 = 7/2 = 3.5, domain Q, exact. */
  {
    SlateNum *three = slate_num_int(cx, 3), *half = slate_num_rat(cx, 1, 2);
    SlateNum *s = slate_num_add(three, half);
    check(slate_num_domain(s) == DOM_Q, "3 + 1/2 domain is Q");
    dec(s, buf, sizeof buf, &exact);
    check(exact && strcmp(buf, "3.5") == 0, "3 + 1/2 reads exact 3.5");
    slate_num_free(three); slate_num_free(half); slate_num_free(s);
  }

  /* 2) an irrational op promotes to ℝ: sqrt(2) is a bracket, not exact, sign +1. */
  {
    SlateNum *two = slate_num_int(cx, 2), *r2 = slate_num_sqrt(two);
    check(slate_num_domain(r2) == DOM_R, "sqrt(2) domain is R");
    check(!slate_num_is_exact(r2), "sqrt(2) is not exact");
    int decided = 0;
    check(slate_num_sign(r2, &decided) == 1 && decided, "sqrt(2) sign +1, decided");
    dec(r2, buf, sizeof buf, &exact);
    check(!exact && strncmp(buf, "1.4142", 6) == 0, "sqrt(2) ~ 1.4142...");
    slate_num_free(two); slate_num_free(r2);
  }

  /* 3) the imaginary unit: exact Gaussian arithmetic. (1 + i)^2 = 2i (re 0, im 2). */
  {
    SlateNum *i = slate_num_i(cx), *one = slate_num_int(cx, 1);
    SlateNum *opi = slate_num_add(one, i);              /* 1 + i */
    SlateNum *sq = slate_num_mul(opi, opi);             /* (1+i)^2 */
    check(slate_num_domain(sq) == DOM_C, "(1+i)^2 domain is C");
    SlateNum *re = slate_num_re(sq), *im = slate_num_im(sq);
    dec(re, buf, sizeof buf, &exact); check(exact && strcmp(buf, "0") == 0, "Re((1+i)^2) = 0 exact");
    dec(im, buf, sizeof buf, &exact); check(exact && strcmp(buf, "2") == 0, "Im((1+i)^2) = 2 exact");
    slate_num_free(i); slate_num_free(one); slate_num_free(opi); slate_num_free(sq);
    slate_num_free(re); slate_num_free(im);
  }

  /* 4) canonical() earns the way down: pi + i is C; its real part canonicalizes to the pi bracket (R); a
   *    Gaussian sum whose imaginary part cancels canonicalizes to ℚ. */
  {
    SlateNum *i = slate_num_i(cx), *pi = slate_num_pi(cx);
    SlateNum *z = slate_num_add(pi, i);                 /* pi + i, in C */
    check(slate_num_domain(z) == DOM_C, "pi + i is C");
    SlateNum *sum = slate_num_sub(z, i);                /* (pi + i) - i */
    SlateNum *canon = slate_num_canonical(sum);
    check(slate_num_domain(canon) == DOM_R, "canonical((pi+i)-i) is R (pi)");
    dec(canon, buf, sizeof buf, &exact);
    check(!exact && strncmp(buf, "3.14159", 7) == 0, "canonical value ~ 3.14159...");
    slate_num_free(i); slate_num_free(pi); slate_num_free(z); slate_num_free(sum); slate_num_free(canon);
  }

  /* 5) the bottom crosses the ABI cleanly: 1/0 is undefined, never a wrong value. */
  {
    SlateNum *one = slate_num_int(cx, 1), *zero = slate_num_int(cx, 0);
    SlateNum *bad = slate_num_div(one, zero);
    check(slate_num_is_undefined(bad), "1/0 is undefined");
    slate_num_free(one); slate_num_free(zero); slate_num_free(bad);
  }

  /* 6) the grid bridge: dispatch z[i] = x[i]*2 + 1 over a grid, then read a cell back as a Number. */
  {
    SlateDag *d = slate_dag_new();
    int64_t xs[] = {0, 1, 2, 3};
    uint32_t cX = slate_dag_carrier(d, xs, 4);
    int32_t i = slate_dag_param(d, 0);
    int32_t root = slate_dag_add(d, slate_dag_mul(d, slate_dag_load(d, cX, i), slate_dag_lit(d, 2)),
                                 slate_dag_lit(d, 1));                        /* 2x + 1 */
    int64_t dims[] = {4};
    SlateArray *arr = slate_dag_run(d, root, dims, 1);
    check(arr != NULL, "grid dispatch produced a result");
    SlateNum *cell2 = slate_num_from_array(cx, arr, 2);                       /* 2*2 + 1 = 5 */
    check(slate_num_domain(cell2) == DOM_N, "grid cell reads back as a Number in N");
    dec(cell2, buf, sizeof buf, &exact);
    check(exact && strcmp(buf, "5") == 0, "grid cell 2 of (2x+1) reads exact 5");
    /* it composes with the scalar surface: cell + 1/2 promotes to Q. */
    SlateNum *half = slate_num_rat(cx, 1, 2), *mixed = slate_num_add(cell2, half);
    check(slate_num_domain(mixed) == DOM_Q, "a grid Number composes with a rational (-> Q)");
    slate_num_free(cell2); slate_num_free(half); slate_num_free(mixed);
    slate_array_free(arr); slate_dag_free(d);
  }

  /* slate_num_fraction: the exact ℚ ratio as little-endian limbs, and a clean refusal for ℝ/ℂ. */
  {
    int32_t sign;
    uint64_t num[4], den[4], nn, nd;

    SlateNum *a = slate_num_int(cx, 3), *b = slate_num_rat(cx, 1, 2);
    SlateNum *q = slate_num_add(a, b);                                        /* 7/2 */
    nn = nd = 4;
    check(slate_num_fraction(q, 256, &sign, num, &nn, den, &nd) == SLATE_BATCH_OK, "fraction of 7/2 ok");
    check(sign == 0 && nn == 1 && nd == 1 && num[0] == 7 && den[0] == 2, "7/2 is +7 over 2");
    slate_num_free(a); slate_num_free(b); slate_num_free(q);

    SlateNum *neg = slate_num_int(cx, -7);
    nn = nd = 4;
    check(slate_num_fraction(neg, 256, &sign, num, &nn, den, &nd) == SLATE_BATCH_OK, "fraction of -7 ok");
    check(sign == 1 && nn == 1 && nd == 1 && num[0] == 7 && den[0] == 1, "-7 is -7 over 1 (sign on the numerator)");
    slate_num_free(neg);

    SlateNum *frac = slate_num_rat(cx, 3, 4);                                 /* size query: NULL buffers */
    nn = nd = 0;
    check(slate_num_fraction(frac, 256, &sign, NULL, &nn, NULL, &nd) == SLATE_BATCH_OK, "fraction size query ok");
    check(nn == 1 && nd == 1, "size query reports the limb counts");
    slate_num_free(frac);

    SlateNum *r2 = slate_num_sqrt(slate_num_int(cx, 2));                      /* ℝ enclosure: no single fraction */
    nn = nd = 4;
    check(slate_num_fraction(r2, 256, &sign, num, &nn, den, &nd) == SLATE_BATCH_EREFUSED, "sqrt(2) has no fraction");
    slate_num_free(r2);

    SlateNum *im = slate_num_i(cx);                                          /* ℂ: no single fraction */
    nn = nd = 4;
    check(slate_num_fraction(im, 256, &sign, num, &nn, den, &nd) == SLATE_BATCH_EREFUSED, "i has no fraction");
    slate_num_free(im);
  }

  /* slate_num_ctx_new_sized: a caller-chosen lazy-DAG op reserve. A tiny reserve forces reads more often
   * but must give the identical exact answer, and 0 must mean "the default reserve" (never a 0-op arena). */
  {
    SlateNumCtx *small = slate_num_ctx_new_sized(8);   /* force after just 8 deferred ops */
    check(small != NULL, "sized ctx (cap 8) allocates");
    if (small) {
      SlateNum *two = slate_num_int(small, 2), *r2 = slate_num_sqrt(two);
      char buf[64]; int ex = 0;
      dec(r2, buf, sizeof buf, &ex);
      check(slate_num_domain(r2) == DOM_R, "sized-ctx sqrt(2) is R");
      check(strncmp(buf, "1.41421356", 10) == 0, "sized-ctx sqrt(2) reads the same digits under a tiny reserve");
      slate_num_free(two); slate_num_free(r2);
      slate_num_ctx_free(small);
    }

    SlateNumCtx *deflt = slate_num_ctx_new_sized(0);   /* 0 => the default reserve, like slate_num_ctx_new */
    check(deflt != NULL, "sized ctx (cap 0 => default) allocates");
    if (deflt) {
      SlateNum *p = slate_num_pi(deflt), *im = slate_num_i(deflt), *z = slate_num_add(p, im);
      check(slate_num_domain(z) == DOM_C, "sized-ctx pi + i is C");
      slate_num_free(p); slate_num_free(im); slate_num_free(z);
      slate_num_ctx_free(deflt);
    }
  }

  slate_num_ctx_free(cx);
  printf("%s test_number_cabi: the Number tower from pure C — Q exact, sqrt promotes to R, i gives exact "
         "Gaussian arithmetic, canonical earns the way down, the bottom crosses cleanly, and a dispatched grid "
         "reads back as Numbers\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
