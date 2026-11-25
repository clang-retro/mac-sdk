/* Checks the C library (runtime/build-libc.sh): stdio to the console and
 * to files, stdin, seeking, rename (also between folders) and remove,
 * stat, fstat, ftruncate, utime, mkdir, rmdir, opendir, getcwd,
 * malloc, realloc and the aligned allocations, setjmp, qsort, strtod,
 * printf's conversions, the time zone and static constructors. It is
 * over 32 KB of 68k code, so tools/macapp.py builds it with
 * -mcmodel=large. tests/run.sh runs the 68k build in tools/sim68k.py
 * --os, which keeps the files in memory, and checks the console output
 * it leaves ("libc-68k Output"): one "ok" line per check, then "all ok".
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#include <OSUtils.h>

static int failures;

static void check(const char *what, int ok) {
  printf("%s %s\n", ok ? "ok" : "FAILED", what);
  if (!ok)
    failures++;
}

static int cmp(const void *a, const void *b) {
  return *(const int *)a - *(const int *)b;
}

static volatile int constructed;   /* volatile: clang may not fold the constructor away */
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
  f = fopen("sub/a.txt", "w");
  fputs("moved\n", f);
  fclose(f);
  fclose(fopen("sub/keep.txt", "w"));
  check("rename out of a folder", rename("sub/a.txt", "b.txt") == 0 &&
                                      !fopen("sub/a.txt", "r") && (f = fopen("b.txt", "r")) &&
                                      fgets(buf, sizeof buf, f) && strcmp(buf, "moved\n") == 0);
  fclose(f);
  check("rename into a folder", rename("b.txt", "sub/c.txt") == 0 && !fopen("b.txt", "r") &&
                                    (f = fopen("sub/c.txt", "r")) != NULL);
  fclose(f);
  remove("sub/c.txt");
  remove("sub/keep.txt");
  {
    struct stat st;
    struct utimbuf ut = {1000000000, 1000000000};   /* September 2001 */
    struct dirent *e;
    DIR *d;
    int seen = 0, fd;
    check("mkdir", mkdir("dir", 0755) == 0 && mkdir("dir", 0755) != 0 && errno == EEXIST);
    check("stat folder", stat("dir", &st) == 0 && S_ISDIR(st.st_mode));
    f = fopen("dir/one.txt", "w");
    fputs("12345", f);
    fclose(f);
    fclose(fopen("dir/two.txt", "w"));
    check("stat file", stat("dir/one.txt", &st) == 0 && S_ISREG(st.st_mode) &&
                           st.st_size == 5 && st.st_mtime > 1700000000);
    check("stat enoent", stat("dir/none", &st) != 0 && errno == ENOENT);
    check("utime", utime("dir/one.txt", &ut) == 0 && stat("dir/one.txt", &st) == 0 &&
                       st.st_mtime == 1000000000);
    f = fopen("dir/one.txt", "r+");
    fd = fileno(f);
    check("fstat", fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 5);
    check("ftruncate", ftruncate(fd, 2) == 0 && fstat(fd, &st) == 0 && st.st_size == 2);
    fclose(f);
    d = opendir("dir");
    check("opendir", d != NULL);
    while (d && (e = readdir(d)) != NULL)
      seen |= strcmp(e->d_name, "one.txt") == 0 ? 1 : strcmp(e->d_name, "two.txt") == 0 ? 2 : 4;
    check("readdir", seen == 3);
    if (d)
      closedir(d);
    check("rmdir not empty", rmdir("dir") != 0 && errno == ENOTEMPTY);
    remove("dir/one.txt");
    remove("dir/two.txt");
    check("rmdir", rmdir("dir") == 0 && stat("dir", &st) != 0);
    check("getcwd", getcwd(buf, sizeof buf) != NULL && buf[0] == '/');
    printf("cwd %s\n", buf);
  }
  /* tests/run.sh gives the program the input "typed\nline\n". */
  check("stdin", fgets(buf, sizeof buf, stdin) && strcmp(buf, "typed\n") == 0 &&
                     fgets(buf, sizeof buf, stdin) && strcmp(buf, "line\n") == 0 &&
                     !fgets(buf, sizeof buf, stdin));

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
  {
    void *q = NULL, *bad = NULL;
    size_t three = 3;   /* not a constant, which clang would warn about */
    char *r = aligned_alloc(256, 100);
    check("aligned_alloc", r && !((uintptr_t)r & 255));
    r[99] = 7;
    r = realloc(r, 3000);
    check("realloc aligned", r && r[99] == 7);
    free(r);
    check("posix_memalign", posix_memalign(&q, 64, 10) == 0 && !((uintptr_t)q & 63) &&
                                posix_memalign(&bad, 6, 10) == EINVAL && !bad);
    free(q);
    check("aligned_alloc 3", aligned_alloc(three, 10) == NULL && errno == EINVAL);
  }
  {
    /* tools/sim68k.py --os is a Mac in UTC-8, whose clock is local time. */
    time_t t = time(NULL);
    unsigned long mac;
    struct tm g = *gmtime(&t), l = *localtime(&t);
    GetDateTime(&mac);
    check("time", t > 1700000000);   /* after 2023 */
    check("time zone", (long)(mac - 2082844800UL - (unsigned long)t) / 60 == -480 &&
                           (g.tm_hour - l.tm_hour + 24) % 24 == 8);
  }
  fprintf(stderr, "stderr too\n");
  printf(failures ? "FAILED\n" : "all ok\n");
  return 0;
}
