/* POSIX functions that a program ported from Unix calls, for the classic
 * Mac OS: chdir, realpath, readlink, uname, gethostname, the user IDs,
 * sysconf, chown, fchown, fchmod, getgroups, flock, poll, ioctl,
 * sigaction, the terminal functions, the process functions (fork,
 * waitpid, posix_spawn, pause) and the dynamic loader functions.
 *
 * The Mac has one user, no processes for a program to start, no
 * terminals, no pipes and no dynamic loader for these programs. The
 * functions for these things fail with the usual error, so that a
 * program can find out and continue (docs/posix.md).
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <Gestalt.h>
#include "macos.h"

/* The ioctl request for the number of bytes that a read can get, as on
   Linux. */
#ifndef FIONREAD
#define FIONREAD 0x541B
#endif

/* The sysconf names for the processor count, with Linux's numbers:
   picolibc does not have them. */
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* Sets errno for a File Manager error and gives -1. */
static int fail(OSErr err) {
  errno = __mac_errno(err);
  return -1;
}

/* Makes the folder at path the current folder. */
int chdir(const char *path) {
  CInfoPBRec pb;
  WDPBRec wd;
  Str255 name;
  OSErr err = __mac_cat_info(path, &pb, name);
  if (err != noErr)
    return fail(err);
  if (!(pb.hFileInfo.ioFlAttrib & 0x10)) {
    errno = ENOTDIR;
    return -1;
  }
  memset(&wd, 0, sizeof wd);
  wd.ioVRefNum = pb.dirInfo.ioVRefNum;
  wd.ioWDDirID = pb.dirInfo.ioDrDirID;
  err = PBHSetVolSync(&wd);
  return err == noErr ? 0 : fail(err);
}

/* Puts the absolute POSIX path of an item that exists in resolved (of
   PATH_MAX bytes), or in a new block from malloc when resolved is NULL.
   The path has no "." or ".." parts. The Mac has no symbolic links, so
   nothing else changes. */
char *realpath(const char *restrict path, char *restrict resolved) {
  char *out = resolved;
  size_t n = 0;
  const char *p = path;
  struct stat st;
  if (!path) {
    errno = EINVAL;
    return NULL;
  }
  if (!*path) {
    errno = ENOENT;
    return NULL;
  }
  if (stat(path, &st) != 0)
    return NULL;
  if (!out && !(out = malloc(PATH_MAX))) {
    errno = ENOMEM;
    return NULL;
  }
  if (*p != '/') {
    if (!getcwd(out, PATH_MAX))
      goto failed;
    n = strlen(out);
  }
  while (*p) {
    size_t len = 0;
    while (*p == '/')
      p++;
    while (p[len] && p[len] != '/')
      len++;
    if (len == 0 || (len == 1 && p[0] == '.')) {
      /* nothing */
    } else if (len == 2 && p[0] == '.' && p[1] == '.') {
      /* Up one level, but not above the volume. */
      size_t at = n;
      while (at > 0 && out[at - 1] != '/')
        at--;
      if (at > 1)
        n = at - 1;
    } else {
      if (n + 1 + len + 1 > PATH_MAX) {
        errno = ENAMETOOLONG;
        goto failed;
      }
      out[n++] = '/';
      memcpy(out + n, p, len);
      n += len;
    }
    p += len;
  }
  if (n == 0)
    out[n++] = '/';
  out[n] = 0;
  return out;
failed:
  if (!resolved)
    free(out);
  return NULL;
}

/* The Mac has no symbolic links: an alias file is a file. Thus no path
   is a link. */
ssize_t readlink(const char *restrict path, char *restrict buf, size_t size) {
  struct stat st;
  (void)buf;
  (void)size;
  if (stat(path, &st) == 0)
    errno = EINVAL;
  return -1;
}

/* The one user of the Mac is user 0. */
uid_t getuid(void) { return 0; }

/* The one user of the Mac is user 0. */
uid_t geteuid(void) { return 0; }

/* The one user of the Mac is in group 0. */
gid_t getgid(void) { return 0; }

/* The one user of the Mac is in group 0. */
gid_t getegid(void) { return 0; }

/* Gives the name of the computer: "Macintosh". */
int gethostname(char *name, size_t len) {
  static const char host[] = "Macintosh";
  if (len < sizeof host) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(name, host, sizeof host);
  return 0;
}

/* Fills in the system names: "MacOS", the host name, the System's
   version from Gestalt ("7.5.3"), and the CPU ("m68k" or "powerpc"). */
int uname(struct utsname *u) {
  long v = 0;
  if (Gestalt(gestaltSystemVersion, &v) != noErr)
    v = 0;
  memset(u, 0, sizeof *u);
  strcpy(u->sysname, "MacOS");
  gethostname(u->nodename, sizeof u->nodename);
  snprintf(u->release, sizeof u->release, "%lx.%lx.%lx", (v >> 8) & 0xFF,
           (v >> 4) & 0xF, v & 0xF);
  strcpy(u->version, u->release);
#ifdef __m68k__
  strcpy(u->machine, "m68k");
#else
  strcpy(u->machine, "powerpc");
#endif
  return 0;
}

/* The values of the system: one processor, and the C library's values
   for the rest. */
long sysconf(int name) {
  switch (name) {
  case _SC_NPROCESSORS_ONLN:
  case _SC_NPROCESSORS_CONF:
    return 1;
  default:
    return __fallback_sysconf(name);
  }
}

/* Locks a file for this program: there is no other program that uses
   the lock, so the lock always succeeds. */
int flock(int fd, int op) {
  (void)op;
  if (fd >= 3 && !__mac_fd_ref(fd)) {
    errno = EBADF;
    return -1;
  }
  return 0;
}

/* Tells which descriptors are ready. Files and the console are always
   ready: a read of the console waits for a line. When none is ready,
   poll waits for timeout milliseconds (other threads run) and gives 0. */
int poll(struct pollfd *fds, nfds_t n, int timeout) {
  nfds_t i;
  int ready = 0;
  for (i = 0; i < n; i++) {
    int fd = fds[i].fd;
    fds[i].revents = 0;
    if (fd < 0)
      continue;
    if (fd < 3 || __mac_fd_ref(fd))
      fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
    else
      fds[i].revents = POLLNVAL;
    if (fds[i].revents)
      ready++;
  }
  if (!ready && timeout > 0) {
    struct timespec ts;
    ts.tv_sec = timeout / 1000;
    ts.tv_nsec = (timeout % 1000) * 1000000L;
    nanosleep(&ts, NULL);
  }
  return ready;
}

/* Does the ioctl request FIONREAD (the bytes after the mark) for an open
   file. There are no devices, so the other requests fail with ENOTTY. */
int ioctl(int fd, unsigned long op, void *param) {
  short ref = __mac_fd_ref(fd);
  if (fd >= 3 && !ref) {
    errno = EBADF;
    return -1;
  }
  if (op == FIONREAD && ref) {
    long eof, pos;
    OSErr err = GetEOF(ref, &eof);
    if (err == noErr)
      err = GetFPos(ref, &pos);
    if (err != noErr)
      return fail(err);
    *(int *)param = (int)(eof - pos);
    return 0;
  }
  errno = ENOTTY;
  return -1;
}

/* Sets or gets the action for a signal, over signal(). Only sa_handler
   is used: a signal handler gets only the signal number. */
int sigaction(int sig, const struct sigaction *restrict act,
              struct sigaction *restrict old) {
  _sig_func_ptr was;
  if (act)
    was = signal(sig, act->sa_handler);
  else if ((was = signal(sig, SIG_DFL)) != SIG_ERR)
    signal(sig, was);
  if (was == SIG_ERR)
    return -1;
  if (old) {
    memset(old, 0, sizeof *old);
    old->sa_handler = was;
  }
  return 0;
}

/* There are no terminals: fails with ENOTTY. */
int tcgetattr(int fd, struct termios *t) {
  (void)fd;
  (void)t;
  errno = ENOTTY;
  return -1;
}

/* There are no terminals: fails with ENOTTY. */
int tcsetattr(int fd, int when, const struct termios *t) {
  (void)fd;
  (void)when;
  (void)t;
  errno = ENOTTY;
  return -1;
}

/* There are no terminals: fails with ENOTTY. */
int tcdrain(int fd) {
  (void)fd;
  errno = ENOTTY;
  return -1;
}

/* There are no terminals: fails with ENOTTY. */
int tcflow(int fd, int action) {
  (void)fd;
  (void)action;
  errno = ENOTTY;
  return -1;
}

/* There are no terminals: fails with ENOTTY. */
int tcflush(int fd, int queue) {
  (void)fd;
  (void)queue;
  errno = ENOTTY;
  return -1;
}

/* Gives the input speed in t. */
speed_t cfgetispeed(const struct termios *t) { return t->c_ispeed; }

/* Gives the output speed in t. */
speed_t cfgetospeed(const struct termios *t) { return t->c_ospeed; }

/* Sets the input speed in t. */
int cfsetispeed(struct termios *t, speed_t speed) {
  t->c_ispeed = speed;
  return 0;
}

/* Sets the output speed in t. */
int cfsetospeed(struct termios *t, speed_t speed) {
  t->c_ospeed = speed;
  return 0;
}

/* Sets t to raw mode: no line editing, no echo, 8-bit characters. */
void cfmakeraw(struct termios *t) {
  t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  t->c_oflag &= ~OPOST;
  t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t->c_cflag &= ~(CSIZE | PARENB);
  t->c_cflag |= CS8;
}

/* Gives the owner of the item at path to the one user: it succeeds when
   the item exists. */
int chown(const char *path, uid_t owner, gid_t group) {
  struct stat st;
  (void)owner;
  (void)group;
  return stat(path, &st);
}

/* Gives the owner of an open file to the one user: it succeeds when fd
   is open. */
int fchown(int fd, uid_t owner, gid_t group) {
  struct stat st;
  (void)owner;
  (void)group;
  return fstat(fd, &st);
}

/* Locks an open file when mode has no write permission, and unlocks it
   otherwise (as chmod). */
int fchmod(int fd, mode_t mode) {
  FCBPBRec fcb;
  HParamBlockRec hp;
  Str255 name;
  OSErr err;
  memset(&fcb, 0, sizeof fcb);
  fcb.ioNamePtr = name;
  fcb.ioRefNum = __mac_fd_ref(fd);
  if (!fcb.ioRefNum) {
    if (fd >= 0 && fd < 3)
      return 0;
    errno = EBADF;
    return -1;
  }
  err = PBGetFCBInfoSync(&fcb);
  if (err != noErr)
    return fail(err);
  memset(&hp, 0, sizeof hp);
  hp.fileParam.ioNamePtr = name;
  hp.fileParam.ioVRefNum = fcb.ioFCBVRefNum;
  hp.fileParam.ioDirID = fcb.ioFCBParID;
  err = (mode & 0222) ? PBHRstFLockSync(&hp) : PBHSetFLockSync(&hp);
  return err == noErr ? 0 : fail(err);
}

/* The one user is in no other group. */
int getgroups(int size, gid_t list[]) {
  (void)size;
  (void)list;
  return 0;
}

/* The program is the only process of its session. */
pid_t getsid(pid_t pid) {
  (void)pid;
  return getpid();
}

/* Waits for a signal. No signal comes from outside the program, so pause
   waits for ever; other threads continue to run. */
int pause(void) {
  struct timespec ts = {1, 0};
  for (;;)
    nanosleep(&ts, NULL);
}

/* A program cannot make another process: fails with ENOSYS. */
pid_t fork(void) {
  errno = ENOSYS;
  return -1;
}

/* There are no pipes between programs: fails with ENOSYS. */
int pipe(int fds[2]) {
  (void)fds;
  errno = ENOSYS;
  return -1;
}

/* No program has child processes: fails with ECHILD. */
pid_t waitpid(pid_t pid, int *status, int options) {
  (void)pid;
  (void)status;
  (void)options;
  errno = ECHILD;
  return -1;
}

/* A program cannot start another program here: gives ENOSYS. */
int posix_spawn(pid_t *restrict pid, const char *restrict path,
                const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *restrict attr,
                char *const argv[restrict], char *const envp[restrict]) {
  (void)pid;
  (void)path;
  (void)actions;
  (void)attr;
  (void)argv;
  (void)envp;
  return ENOSYS;
}

/* A program cannot start another program here: gives ENOSYS. */
int posix_spawnp(pid_t *restrict pid, const char *restrict file,
                 const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *restrict attr,
                 char *const argv[restrict], char *const envp[restrict]) {
  return posix_spawn(pid, file, actions, attr, argv, envp);
}

/* Starts an empty list of file actions for posix_spawn. */
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions) {
  *actions = NULL;
  return 0;
}

/* Releases a list of file actions: it holds nothing. */
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions) {
  (void)actions;
  return 0;
}

/* Adds an open action. posix_spawn fails, so nothing is kept. */
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *restrict actions,
                                     int fd, const char *restrict path,
                                     int flags, mode_t mode) {
  (void)actions;
  (void)fd;
  (void)path;
  (void)flags;
  (void)mode;
  return 0;
}

/* Adds a dup2 action. posix_spawn fails, so nothing is kept. */
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions,
                                     int fd, int to) {
  (void)actions;
  (void)fd;
  (void)to;
  return 0;
}

/* Adds a close action. posix_spawn fails, so nothing is kept. */
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions,
                                      int fd) {
  (void)actions;
  (void)fd;
  return 0;
}

/* The handle that dlopen(NULL) gives: the program itself. */
static char program;

/* The reason for the last dlopen failure, or NULL. */
static const char *dl_reason;

/* Gives a handle for the program itself when path is NULL. No library
   can be loaded: a program is one application. */
void *dlopen(const char *path, int mode) {
  (void)mode;
  if (!path)
    return &program;
  dl_reason = "the classic Mac OS has no dynamic libraries";
  return NULL;
}

/* Finds no symbol: the program has no table of its symbols. */
void *dlsym(void *restrict handle, const char *restrict name) {
  (void)handle;
  (void)name;
  dl_reason = "the program has no symbol table";
  return NULL;
}

/* Releases a handle: there is nothing to release. */
int dlclose(void *handle) {
  (void)handle;
  return 0;
}

/* Gives the reason for the last failure once, then NULL. */
char *dlerror(void) {
  const char *r = dl_reason;
  dl_reason = NULL;
  return (char *)r;
}

/* Finds no information for an address. */
int dladdr(const void *addr, Dl_info *info) {
  (void)addr;
  (void)info;
  return 0;
}
