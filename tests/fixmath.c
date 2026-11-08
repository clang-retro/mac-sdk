/* Checks the 68k glue for the Fixed and Fract conversions and the 64-bit
 * wide arithmetic (FixMath.h), which needs the runtime library. Run like
 * tests/runtime.c: each check draws "ok", or its name if it fails.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <FixMath.h>
#include <Quickdraw.h>

static int failures;

static void say(const char *what) {
  Str255 s;
  unsigned char n = 0;
  while (what[n])
    n++;
  s[0] = n;
  for (n = 0; n < s[0]; n++)
    s[n + 1] = what[n];
  DrawString(s);
}

static void report(const char *what, int ok) {
  say(ok ? "ok" : what);
  if (!ok)
    failures++;
}

static int is(const wide *w, long hi, unsigned long lo) {
  return w->hi == hi && w->lo == lo;
}

/* volatile, so clang cannot work the answers out while compiling. */
static volatile Fixed f = 0x00018000;            /* 1.5 */
static volatile Fract fr = 0x20000000;           /* 0.5 */
static volatile double d = -2.25;

int main(void) {
  wide a, b;
  long rem;

  report("Fix2X", Fix2X(f) == 1.5 && Fix2X(-f) == -1.5);
  report("Frac2X", Frac2X(fr) == 0.5);
  report("X2Fix", X2Fix(d) == (Fixed)0xFFFDC000 && X2Fix(1e9) == 0x7FFFFFFF &&
                      X2Fix(-1e9) == (Fixed)0x80000000 &&
                      X2Fix(0.5 / 65536.0) == 0 && X2Fix(1.5 / 65536.0) == 2);
  report("X2Frac", X2Frac(0.25) == 0x10000000 && X2Frac(3.0) == 0x7FFFFFFF);

  a.hi = 0; a.lo = 0xFFFFFFFF;
  b.hi = 1; b.lo = 1;
  report("WideAdd", is(WideAdd(&a, &b), 2, 0));
  report("WideSubtract", is(WideSubtract(&a, &b), 0, 0xFFFFFFFF) &&
                             is(WideSubtract(&a, &b), -1, 0xFFFFFFFE));
  report("WideNegate", is(WideNegate(&a), 0, 2) && is(WideNegate(&a), -1, 0xFFFFFFFE));
  b.hi = -1; b.lo = 0xFFFFFFFF;
  report("WideCompare", WideCompare(&a, &b) < 0 && WideCompare(&b, &a) > 0 &&
                            WideCompare(&a, &a) == 0);
  report("WideMultiply", is(WideMultiply(0x12345678, -16, &a), -2, 0xDCBA9880) &&
                             is(WideMultiply(0x40000000, 4, &b), 1, 0));
  report("WideDivide", WideDivide(&a, -16, &rem) == 0x12345678 && rem == 0 &&
                           WideDivide(&b, 3, &rem) == 0x55555555 && rem == 1 &&
                           WideDivide(&b, 1, &rem) == 0x7FFFFFFF &&
                           rem == (long)0x80000000);
  /* With no remainder the quotient is rounded, halves away from zero. */
  a.hi = 0; a.lo = 7;
  report("WideDivide rounds", WideDivide(&a, 2, 0) == 4 && WideDivide(&a, -2, 0) == -4 &&
                                  WideDivide(&a, 3, 0) == 2 &&
                                  WideDivide(&a, 0, (long *)-1) == (long)0x80000000 &&
                                  WideDivide(&a, 0, 0) == 0x7FFFFFFF);
  b.hi = 7; b.lo = 5;
  report("WideWideDivide", is(WideWideDivide(&b, 2, &rem), 3, 0x80000002) && rem == 1 &&
                               is(WideWideDivide(&b, 4, 0), 0, 0xE0000001) &&
                               is(WideWideDivide(&b, 0, &rem), 0x7FFFFFFF, 0xFFFFFFFF) &&
                               rem == (long)0x80000000);
  a.hi = -1; a.lo = 0xFFFFFFFB;                    /* -5 */
  report("WideBitShift", is(WideBitShift(&a, 1), -1, 0xFFFFFFFD) &&
                             is(WideBitShift(&a, -33), -6, 0) &&
                             is(WideBitShift(&a, 100), -1, 0xFFFFFFFF));
  a.hi = 0; a.lo = 6;
  report("WideShift", is(WideShift(&a, 2), 0, 2) && is(WideShift(&a, -40), 0x200, 0) &&
                          is(WideShift(&a, 41), 0, 1));
  a.hi = -1; a.lo = 0xFFFFFFFA;                    /* -6: -1.5 rounds to -1 */
  report("WideShift negative", is(WideShift(&a, 2), -1, 0xFFFFFFFF));
  a.hi = 0; a.lo = 0;
  b.hi = 0; b.lo = 1000001;
  report("WideSquareRoot", WideSquareRoot(&a) == 0 && WideSquareRoot(&b) == 1000 &&
                               (b.lo = 1000 * 1000 + 1000, WideSquareRoot(&b)) == 1000 &&
                               (b.lo = 1000 * 1000 + 1001, WideSquareRoot(&b)) == 1001);
  b.hi = -1; b.lo = 0xFFFFFFFF;
  report("WideSquareRoot max", WideSquareRoot(&b) == 0xFFFFFFFF);
  b.hi = 0x3FFFFFFF; b.lo = 0;
  report("WideSquareRoot big", WideSquareRoot(&b) == 0x7FFFFFFF);
  say(failures ? "FAILED" : "all ok");
  return 0;
}
