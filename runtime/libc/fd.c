/* File descriptors over the File Manager: open, close, read, write and
 * lseek. Descriptors 0, 1 and 2 are the console (stdin reads nothing;
 * stdout and stderr go to console.c); the others are open data forks.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include "macos.h"

/* The file type and creator that open(..., O_CREAT) gives a new file, as
   in MPW's C library: a TEXT file that SimpleText opens. A program may
   change them before it creates a file. */
OSType _ftype = 'TEXT';
OSType _fcreator = 'ttxt';

#define NFD 32

static struct {
  short ref;
  short flags;          /* the open() flags; 0 when the slot is free */
} fds[NFD];

static int bad(int fd) {
  if (fd < 3 || fd >= NFD || !fds[fd].flags) {
    errno = EBADF;
    return 1;
  }
  return 0;
}

static int fail(OSErr err) {
  errno = __mac_errno(err);
  return -1;
}

int open(const char *path, int flags, ...) {
  Str255 name;
  ParamBlockRec pb;
  OSErr err;
  int fd, acc = flags & O_ACCMODE;

  if (__mac_path(path, name) < 0)
    return -1;
  for (fd = 3; fd < NFD && fds[fd].flags; fd++)
    ;
  if (fd == NFD) {
    errno = EMFILE;
    return -1;
  }
  if (flags & O_CREAT) {
    err = Create(name, 0, _fcreator, _ftype);
    if (err == dupFNErr) {
      if (flags & O_EXCL)
        return fail(err);
    } else if (err != noErr) {
      return fail(err);
    }
  }
  pb.ioParam.ioCompletion = 0;
  pb.ioParam.ioNamePtr = name;
  pb.ioParam.ioVRefNum = 0;
  pb.ioParam.ioVersNum = 0;
  pb.ioParam.ioPermssn = acc == O_RDONLY ? fsRdPerm
                         : acc == O_WRONLY ? fsWrPerm : fsRdWrPerm;
  pb.ioParam.ioMisc = 0;
  err = PBOpenSync(&pb);
  if (err != noErr)
    return fail(err);
  if ((flags & O_TRUNC) && acc != O_RDONLY) {
    err = SetEOF(pb.ioParam.ioRefNum, 0);
    if (err != noErr) {
      FSClose(pb.ioParam.ioRefNum);
      return fail(err);
    }
  }
  fds[fd].ref = pb.ioParam.ioRefNum;
  fds[fd].flags = (short)(flags | 0x4000);   /* nonzero even for O_RDONLY */
  return fd;
}

int close(int fd) {
  OSErr err;
  if (fd >= 0 && fd < 3)
    return 0;
  if (bad(fd))
    return -1;
  err = FSClose(fds[fd].ref);
  fds[fd].flags = 0;
  return err == noErr ? 0 : fail(err);
}

ssize_t read(int fd, void *buf, size_t n) {
  long count = (long)n;
  OSErr err;
  if (fd == 0)
    return 0;                   /* no console input */
  if (bad(fd))
    return -1;
  if ((fds[fd].flags & O_ACCMODE) == O_WRONLY) {
    errno = EBADF;
    return -1;
  }
  err = FSRead(fds[fd].ref, &count, buf);
  if (err != noErr && err != eofErr)
    return fail(err);
  return count;
}

ssize_t write(int fd, const void *buf, size_t n) {
  long count = (long)n;
  OSErr err;
  if (fd == 1 || fd == 2) {
    __mac_console_write(buf, n);
    return (ssize_t)n;
  }
  if (bad(fd))
    return -1;
  if ((fds[fd].flags & O_ACCMODE) == O_RDONLY) {
    errno = EBADF;
    return -1;
  }
  if (fds[fd].flags & O_APPEND) {
    err = SetFPos(fds[fd].ref, fsFromLEOF, 0);
    if (err != noErr)
      return fail(err);
  }
  err = FSWrite(fds[fd].ref, &count, buf);
  if (err != noErr)
    return fail(err);
  return count;
}

off_t lseek(int fd, off_t offset, int whence) {
  short ref;
  long pos, eof;
  OSErr err;
  if (fd >= 0 && fd < 3) {
    errno = ESPIPE;
    return -1;
  }
  if (bad(fd))
    return -1;
  ref = fds[fd].ref;
  /* Work out the new mark, so that a seek past the end can grow the file
     as POSIX says. */
  err = GetEOF(ref, &eof);
  if (err == noErr)
    err = GetFPos(ref, &pos);
  if (err != noErr)
    return fail(err);
  if (whence == SEEK_SET)
    pos = 0;
  else if (whence == SEEK_END)
    pos = eof;
  else if (whence != SEEK_CUR) {
    errno = EINVAL;
    return -1;
  }
  if (offset > 0x7FFFFFFF - pos || pos + offset < 0) {
    errno = EINVAL;
    return -1;
  }
  pos += (long)offset;
  if (pos > eof) {
    if ((fds[fd].flags & O_ACCMODE) == O_RDONLY) {
      errno = EINVAL;
      return -1;
    }
    err = SetEOF(ref, pos);
    if (err != noErr)
      return fail(err);
  }
  err = SetFPos(ref, fsFromStart, pos);
  if (err != noErr)
    return fail(err);
  return pos;
}

int isatty(int fd) {
  if (fd >= 0 && fd < 3)
    return 1;
  errno = ENOTTY;
  return 0;
}
