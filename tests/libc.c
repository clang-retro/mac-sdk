/* Checks the C library (runtime/build-libc.sh): stdio to the console and
 * to files, seeking, rename and remove, malloc and realloc, setjmp,
 * qsort, strtod and printf's conversions. tests/run.sh runs the 68k build
 * in tools/sim68k.py --os, which keeps the files in memory, and checks
 * the console output it leaves ("libc-68k Output"): one "ok" line per check,
 * then "all ok".
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;

static void check(const char *what, int ok) {
  printf("%s %s\n", ok ? "ok" : "FAILED", what);
  if (!ok)
    failures++;
}

static int cmp(const void *a, const void *b) {
  return *(const int *)a - *(const int *)b;
}

static int constructed;
__attribute__((constructor)) static void construct(void) { constructed = 42; }

int main(int argc, char **argv) {
  char buf[64];
  FILE *f;
  int v[5] = {5, 3, 4, 1, 2}, i, n;
  char *p;
  jmp_buf env;
  volatile int jumped = 0;

  check("argv", argc == 1 && strncmp(argv[0], "libc", 4) == 0);   /* the name */
  check("constructor", constructed == 42);

  snprintf(buf, sizeof buf, "%d|%5.2f|%-4s|%x|%ld", -12, 3.14159, "ab", 255u,
           123456789L);
  check("printf", strcmp(buf, "-12| 3.14|ab  |ff|123456789") == 0);
  check("strtod", strtod("2.5e3", NULL) == 2500.0 && atoi("-77") == -77);

  f = fopen("data.txt", "w");
  check("fopen w", f != NULL);
  fprintf(f, "line one\nline %d\n", 2);
  check("fclose", fclose(f) == 0);
  f = fopen("data.txt", "r");
  check("fgets", f && fgets(buf, sizeof buf, f) && strcmp(buf, "line one\n") == 0 &&
                     fgets(buf, sizeof buf, f) && strcmp(buf, "line 2\n") == 0 &&
                     !fgets(buf, sizeof buf, f));
  check("fseek", fseek(f, 5, SEEK_SET) == 0 && fgetc(f) == 'o' && ftell(f) == 6);
  fclose(f);
  f = fopen("data.txt", "a");
  fputs("three\n", f);
  fclose(f);
  f = fopen("data.txt", "rb");
  n = (int)fread(buf, 1, sizeof buf, f);
  fclose(f);
  check("append", n == 22 && memcmp(buf + 16, "three\n", 6) == 0);
  check("rename", rename("data.txt", "moved.txt") == 0 && !fopen("data.txt", "r"));
  check("enoent", fopen("nothing", "r") == NULL && errno == ENOENT);
  check("remove", remove("moved.txt") == 0 && remove("moved.txt") != 0);

  p = malloc(100);
  for (i = 0; i < 100; i++)
    p[i] = (char)i;
  p = realloc(p, 5000);
  check("malloc", p && p[99] == 99 && calloc(4, 4) != NULL);
  free(p);

  if (setjmp(env) == 0) {
    jumped = 1;
    longjmp(env, 7);
  }
  check("setjmp", jumped == 1);

  qsort(v, 5, sizeof v[0], cmp);
  check("qsort", v[0] == 1 && v[4] == 5);
  check("time", time(NULL) > 1700000000);   /* after 2023 */
  fprintf(stderr, "stderr too\n");
  printf(failures ? "FAILED\n" : "all ok\n");
  return 0;
}
