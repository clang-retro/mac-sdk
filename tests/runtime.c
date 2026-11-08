/* Checks the runtime library (runtime/build.sh): the builtins clang calls
 * for 32- and 64-bit arithmetic and soft float, and the memory and string
 * functions. tests/run.sh links it for both CPUs and runs the 68k build in
 * tools/sim68k.py, which prints each DrawString: "ok" for each check that
 * passes, and the check's name for one that fails.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <Quickdraw.h>
#include <stddef.h>

/* There is no C library yet; these are in the runtime library. */
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t);
size_t strlen(const char *);
char *strcpy(char *, const char *);
char *strcat(char *, const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strchr(const char *, int);
char *strrchr(const char *, int);

/* volatile, so clang cannot work the answers out while compiling. */
static volatile long a32 = -1234567, b32 = 89;
static volatile unsigned long ua = 4000000000UL, ub = 7;
static volatile long long a64 = -123456789012345LL, b64 = 10007;
static volatile double da = 2.5, db = -0.75;
static volatile float fa = 3.0f, fb = 0.5f;

struct big { char c[61]; long l; };

static int failures;

static void say(const char *what) {
  Str255 s;
  size_t n = strlen(what);
  s[0] = (unsigned char)n;
  memcpy(s + 1, what, n);
  DrawString(s);
}

static void report(const char *what, int ok) {
  say(ok ? "ok" : what);
  if (!ok)
    failures++;
}

#define MUL64 (-1235432087646536415LL)
#define DIV64 (-12337042971LL)
#define MOD64 (-1548)

int main(void) {
  struct big x, y;
  char buf[32];

  report("mul32", a32 * b32 == -109876463);
  report("div32", a32 / b32 == -13871 && a32 % b32 == -48);
  report("udiv32", ua / ub == 571428571UL && ua % ub == 3);
  report("mul64", a64 * b64 == MUL64);
  report("div64", a64 / b64 == DIV64 && a64 % b64 == MOD64);
  report("shift64", (a64 << 13) >> 13 == a64 && ((unsigned long long)ua << 20) >> 20 == ua);
  report("double", da * db == -1.875 && da / db < -3.33 && da / db > -3.34 &&
                       da + db == 1.75 && da - db == 3.25);
  report("float", fa * fb == 1.5f && fa / fb == 6.0f && fa - fb == 2.5f);
  report("convert", (long)(da * 1000.0) == 2500 && (double)b32 == 89.0 &&
                        (long long)(da * 1e12) == 2500000000000LL &&
                        (float)da == 2.5f && (unsigned long)(fa * 1e9f) == 3000000000UL);
  memset(&x, 0x5A, sizeof x);
  x.l = 42;
  y = x;
  report("struct", memcmp(&x, &y, sizeof x) == 0 && y.l == 42 && y.c[60] == 0x5A);
  strcpy(buf, "Hello, ");
  strcat(buf, "Mac");
  report("string", strlen(buf) == 10 && strcmp(buf, "Hello, Mac") == 0 &&
                       strncmp(buf, "Help", 3) == 0 && strchr(buf, 'M') == buf + 7 &&
                       strrchr(buf, 'l') == buf + 3 && memchr(buf, ',', 10) == buf + 5);
  memmove(buf + 1, buf, 5);
  report("memmove", memcmp(buf, "HHello", 6) == 0);
  say(failures ? "FAILED" : "all ok");
  return 0;
}
