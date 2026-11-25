/* Checks the C library (runtime/build-libc.sh): stdio to the console and
 * to files, stdin, seeking, rename (also between folders) and remove,
 * stat, fstat, ftruncate, utime, mkdir, rmdir, opendir, getcwd, the
 * POSIX additions (dup, fcntl, pread, pwrite, fsync, access, chmod,
 * futimens, chdir, realpath, mmap, shm_open, poll, ioctl, uname, dlopen
 * and the functions that fail), malloc, realloc and the aligned allocations, setjmp, qsort, strtod,
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
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <utime.h>
#include <sys/stat.h>
#include <OSUtils.h>

int shm_open(const char *name, int flags, mode_t mode);
int shm_unlink(const char *name);

static int failures;

/* Prints one check's result, and counts the failures. */
static void check(const char *what, int ok) {
  printf("%s %s\n", ok ? "ok" : "FAILED", what);
  if (!ok)
    failures++;
}

/* The ioctl request of the C library (runtime/libc/posix.c). */
#ifndef FIONREAD
#define FIONREAD 0x541B
#endif

/* The sysconf name of the processor count (runtime/libc/posix.c). */
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif

/* Checks the POSIX functions of runtime/libc/fd.c, stat.c, posix.c and
   mman.c. */
static void check_posix(void) {
  char buf[256];
  struct stat st;
  int fd, fd2, n = 0;
  char *m, *m2;
  fd = open("px.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
  check("open rdwr", fd >= 3 && write(fd, "abcdefgh", 8) == 8);
  fd2 = dup(fd);
  check("dup", fd2 > fd && lseek(fd2, 0, SEEK_CUR) == 8);
  check("pread", pread(fd, buf, 3, 2) == 3 && memcmp(buf, "cde", 3) == 0 &&
                     lseek(fd, 0, SEEK_CUR) == 8);
  check("pwrite", pwrite(fd, "XY", 2, 0) == 2 && pread(fd, buf, 3, 0) == 3 &&
                      memcmp(buf, "XYc", 3) == 0);
  check("fcntl", fcntl(fd, F_GETFL) == O_RDWR && fcntl(fd, F_SETFL, O_APPEND) == 0 &&
                     (fcntl(fd, F_GETFL) & O_APPEND) && fcntl(fd, F_SETFL, 0) == 0);
  check("fcntl dupfd", (n = fcntl(fd, F_DUPFD, 20)) >= 20 && close(n) == 0);
  check("close a copy", close(fd2) == 0 && pread(fd, buf, 1, 7) == 1 && buf[0] == 'h');
  check("fsync", fsync(fd) == 0);
  check("ioctl fionread", lseek(fd, 5, SEEK_SET) == 5 && ioctl(fd, FIONREAD, &n) == 0 &&
                              n == 3);
  check("futimens", futimens(fd, NULL) == 0);
  {
    struct pollfd p[2] = {{fd, POLLIN, 0}, {99, POLLIN, 0}};
    check("poll", poll(p, 2, 0) == 2 && p[0].revents == POLLIN && p[1].revents == POLLNVAL);
  }

  /* A shared writable mapping writes back; a second one shares the block. */
  m = mmap(NULL, 8, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  m2 = mmap(NULL, 4, PROT_READ, MAP_SHARED, fd, 4);
  check("mmap file", m != MAP_FAILED && memcmp(m, "XYcdefgh", 8) == 0 && m2 == m + 4);
  m[1] = 'Z';
  check("msync", msync(m, 8, MS_SYNC) == 0 && pread(fd, buf, 2, 0) == 2 &&
                     memcmp(buf, "XZ", 2) == 0);
  m[2] = 'Q';
  check("munmap", munmap(m2, 4) == 0 && munmap(m, 8) == 0 && pread(fd, buf, 3, 0) == 3 &&
                      memcmp(buf, "XZQ", 3) == 0 && munmap(m, 8) != 0);
  close(fd);
  m = mmap(NULL, 1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check("mmap anonymous", m != MAP_FAILED && m[0] == 0 && m[999] == 0 &&
                              !((uintptr_t)m & 15));
  m[10] = 1;
  check("mmap fixed", mmap(m, 100, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == m &&
                          m[10] == 0 && mprotect(m, 1000, PROT_READ) == 0 &&
                          munmap(m, 1000) == 0);
  fd = shm_open("/region", O_RDWR | O_CREAT, 0600);
  check("shm_open", fd >= 3 && ftruncate(fd, 16) == 0 && close(fd) == 0 &&
                        (fd = shm_open("/region", O_RDWR, 0)) >= 3 && close(fd) == 0 &&
                        shm_unlink("/region") == 0 && shm_open("/region", O_RDWR, 0) < 0);

  check("access", access("px.txt", F_OK) == 0 && access("none", F_OK) != 0 && errno == ENOENT);
  check("chmod", chmod("px.txt", 0444) == 0 && access("px.txt", W_OK) != 0 &&
                     errno == EACCES && stat("px.txt", &st) == 0 && !(st.st_mode & 0200) &&
                     chmod("px.txt", 0644) == 0 && access("px.txt", W_OK) == 0);
  check("realpath", realpath("px.txt", buf) && buf[0] == '/' &&
                        strcmp(buf + strlen(buf) - 7, "/px.txt") == 0);
  {
    char cwd[256], *r = realpath(".", NULL);
    check("realpath dots", r && getcwd(cwd, sizeof cwd) && strcmp(r, cwd) == 0);
    free(r);
  }
  check("readlink", readlink("px.txt", buf, sizeof buf) < 0 && errno == EINVAL);
  check("chdir", chdir(".") == 0 && chdir("px.txt") != 0 && errno == ENOTDIR);
  remove("px.txt");
  {
    struct utsname u;
    check("uname", uname(&u) == 0 && strcmp(u.sysname, "MacOS") == 0 &&
                       gethostname(buf, sizeof buf) == 0 && getuid() == 0);
  }
  check("sysconf", sysconf(_SC_NPROCESSORS_ONLN) == 1 && sysconf(_SC_PAGESIZE) > 0);
  check("dlopen", dlopen(NULL, RTLD_NOW) != NULL && dlopen("lib", RTLD_NOW) == NULL &&
                      dlerror() != NULL && dlerror() == NULL);
  {
    int fds[2];
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    check("sigaction", sigaction(SIGPIPE, &sa, &old) == 0 && old.sa_handler == SIG_DFL &&
                           sigaction(SIGPIPE, NULL, &old) == 0 && old.sa_handler == SIG_IGN);
    check("pipe fails", pipe(fds) != 0 && errno == ENOSYS);
    check("waitpid fails", waitpid(1, NULL, 0) < 0 && errno == ECHILD);
  }
  {
    char *p = malloc(40);
    check("malloc_usable_size", p && malloc_usable_size(p) >= 40);
    free(p);
  }
}

/* Compares two ints, for qsort. */
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
  check_posix();
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
