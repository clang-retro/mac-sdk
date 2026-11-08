/* Checks the 68k glue that is not FixMath (tests/fixmath.c has that). Run
 * like tests/runtime.c: each check draws "ok", or its name if it fails.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <OSUtils.h>
#include <Quickdraw.h>
#include <Power.h>
#include <Sound.h>
#include <TextEdit.h>

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

static pascal Boolean breakAtSpace(Ptr text, short charPos) {
  return text[charPos] == ' ';
}

static int clicks;

static pascal Boolean countClicks(void) {
  return ++clicks < 3;
}

/* Call a TextEdit hook as TextEdit does: text in A0, position in D0, the
   answer in the Z flag. Returns 1 for a break (Z clear), 0 for none. */
static int callHook(ProcPtr hook, Ptr text, short charPos) {
  register Ptr a0 __asm__("a0") = text;
  register long d0 __asm__("d0") = charPos;
  char z;
  __asm__ __volatile__("jsr (%3)\n\tseq %1"
                       : "+r"(d0), "=d"(z), "+r"(a0)
                       : "a"(hook)
                       : "d1", "a1", "cc", "memory");
  return !z;
}

static void checkTextEdit(void) {
  TERec rec;
  TEPtr p = &rec;
  TEHandle h = &p;
  char text[] = "ab cd";
  int clickOk;
  TESetWordBreak((ProcPtr)breakAtSpace, h);
  TESetClickLoop((ProcPtr)countClicks, h);
  report("TESetWordBreak", callHook(rec.wordBreak, text, 2) == 1 &&
                               callHook(rec.wordBreak, text, 1) == 0);
  clickOk = callHook(rec.clikLoop, 0, 0) == 1 && callHook(rec.clikLoop, 0, 0) == 1 &&
            callHook(rec.clikLoop, 0, 0) == 0;
  report("TESetClickLoop", clickOk && clicks == 3);
}

/* tools/sim68k.py --os answers _PMgrOp as a PowerBook would. */
static void checkPower(void) {
  Byte status = 0, power = 0, flag = 9;
  long when = 0;
  short vol = 0;
  report("BatteryStatus", BatteryStatus(&status, &power) == noErr && status == 5 &&
                              power == 0xC8);
  report("ModemStatus", ModemStatus(&status) == noErr && status == 10);
  report("SetWUTime", SetWUTime(0x12345678) == noErr && GetWUTime(&when, &flag) == noErr &&
                          when == 0x12345678 && flag == 1);
  report("DisableWUTime", DisableWUTime() == noErr && GetWUTime(&when, &flag) == noErr &&
                              flag == 0);
  SetSoundVol(5);
  GetSoundVol(&vol);
  report("SetSoundVol", vol == 5);
}
#endif

int main(void) {
#if TARGET_CPU_68K
  checkTextEdit();
  checkPower();
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
