/* Checks the 68k glue that is not FixMath (tests/fixmath.c has that). Run
 * like tests/runtime.c: each check draws "ok", or its name if it fails.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <OSUtils.h>
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

#if TARGET_CPU_68K
static void report(const char *what, int ok) {
  say(ok ? "ok" : what);
  if (!ok)
    failures++;
}

static long a5(void) {
  long r;
  __asm__ __volatile__("move.l %%a5, %0" : "=d"(r));
  return r;
}

/* What an interrupt routine does, with A5 not the application's. */
static long nested(void) {
  long seen;
  long old = SetA5(0x2468);
  SetUpA5();
  seen = a5();
  RestoreA5();
  seen = seen == 0x1234 && a5() == 0x2468;
  SetA5(old);
  return seen;
}
#endif

int main(void) {
#if TARGET_CPU_68K
  long app = a5(), cur = *(long *)0x904, inner, outer;
  *(long *)0x904 = 0x1234;
  SetA5(0x5678);
  SetUpA5();
  outer = a5();
  inner = nested();
  RestoreA5();
  report("SetUpA5", outer == 0x1234 && inner && a5() == 0x5678);
  SetA5(app);
  *(long *)0x904 = cur;
#endif
  say(failures ? "FAILED" : "all ok");
  return 0;
}
