/* File descriptors over the File Manager: open, close, read, write,
 * lseek, pread, pwrite, dup, dup2, fcntl and fsync. Descriptors 0, 1 and 2 are the console (stdin reads from
 * input.c, stdout and stderr write to console.c); the others are open
 * data forks, pipes or sockets (see __mac_fd_new).
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/uio.h>
#include "macos.h"

/* The file type and creator that open(..., O_CREAT) gives a new file, as
   in MPW's C library: a TEXT file that SimpleText opens. A program may
   change them before it creates a file. */
OSType _ftype = 'TEXT';
OSType _fcreator = 'ttxt';

#define NFD 32

static struct {
  short ref;
  int flags;            /* the open() flags */
  char used;            /* 0 when the slot is free */
  const struct __mac_fd_ops *ops; /* a pipe's or a socket's, or NULL */
  void *obj;            /* the pipe or the socket */
} fds[NFD];

/* Tells if fd is not an open file, and sets errno to EBADF if so. */
static int bad(int fd) {
  if (fd < 3 || fd >= NFD || !fds[fd].used) {
    errno = EBADF;
    return 1;
  }
  return 0;
}

/* Sets errno for a File Manager error and gives -1. */
static int fail(OSErr err) {
  errno = __mac_errno(err);
  return -1;
}

/* Opens the data fork of a file, and makes it if O_CREAT is set. */
int open(const char *path, int flags, ...) {
  Str255 name;
  ParamBlockRec pb;
  OSErr err;
  int fd, acc = flags & O_ACCMODE;

  if (__mac_path(path, name) < 0)
    return -1;
  {
    /* A named pipe (mkfifo) is a file of type 'fifo' (unix.c). */
    CInfoPBRec ci;
    Str255 ciname;
    if (__mac_cat_info(path, &ci, ciname) == noErr && !(ci.hFileInfo.ioFlAttrib & 0x10) &&
        ci.hFileInfo.ioFlFndrInfo.fdType == 'fifo')
      return __mac_fifo_open(path, flags);
  }
  for (fd = 3; fd < NFD && fds[fd].used; fd++)
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
  fds[fd].flags = flags;
  fds[fd].used = 1;
  return fd;
}

/* Tells if a descriptor other than fd has the same open file, pipe or
   socket (a copy from dup). */
static int shared(int fd) {
  int i;
  for (i = 3; i < NFD; i++)
    if (i != fd && fds[i].used && fds[i].ops == fds[fd].ops &&
        (fds[i].ops ? fds[i].obj == fds[fd].obj : fds[i].ref == fds[fd].ref))
      return 1;
  return 0;
}

/* Closes fd. The file, pipe or socket closes when its last descriptor
   closes. */
int close(int fd) {
  OSErr err = noErr;
  int r = 0;
  if (fd >= 0 && fd < 3)
    return 0;
  if (bad(fd))
    return -1;
  fds[fd].used = 0;
  if (fds[fd].ops) {
    fds[fd].used = 1;
    if (!shared(fd))
      r = fds[fd].ops->close(fds[fd].obj);
    fds[fd].used = 0;
    fds[fd].ops = 0;
    return r;
  }
  fds[fd].used = 1;
  if (!shared(fd))
    err = FSClose(fds[fd].ref);
  fds[fd].used = 0;
  return err == noErr ? 0 : fail(err);
}

/* Gives a new descriptor for an object that is not a file (a pipe or a
   socket). The calls of "ops" read, write and close it. */
int __mac_fd_new(const struct __mac_fd_ops *ops, void *obj, int flags) {
  int fd;
  for (fd = 3; fd < NFD && fds[fd].used; fd++)
    ;
  if (fd == NFD) {
    errno = EMFILE;
    return -1;
  }
  fds[fd].ref = 0;
  fds[fd].flags = flags;
  fds[fd].ops = ops;
  fds[fd].obj = obj;
  fds[fd].used = 1;
  return fd;
}

/* Gives the object of a descriptor, if its calls are "ops" (or any calls,
   for NULL), and its flags. Sets ENOTSOCK for other descriptors. */
void *__mac_fd_obj(int fd, const struct __mac_fd_ops *ops, int *flags) {
  if (fd >= 0 && fd < 3) {
    errno = ENOTSOCK;
    return 0;
  }
  if (bad(fd))
    return 0;
  if (!fds[fd].ops || (ops && fds[fd].ops != ops)) {
    errno = ENOTSOCK;
    return 0;
  }
  if (flags)
    *flags = fds[fd].flags;
  return fds[fd].obj;
}

/* Gives the calls of a descriptor, or NULL for a file. */
const struct __mac_fd_ops *__mac_fd_ops(int fd) {
  if (fd < 3 || fd >= NFD || !fds[fd].used)
    return 0;
  return fds[fd].ops;
}

/* Makes descriptor to (3 or more) a copy of descriptor from. The two
   share the file and its mark. */
static int copy_fd(int from, int to) {
  if (from >= 0 && from < 3) {
    errno = EINVAL;            /* the console has no descriptor slot */
    return -1;
  }
  if (bad(from))
    return -1;
  if (to == from)
    return to;
  if (fds[to].used)
    close(to);
  fds[to] = fds[from];
  return to;
}

/* Gives the lowest free descriptor that is min or more, or -1 with
   EMFILE. */
static int free_fd(int min) {
  int fd;
  for (fd = min < 3 ? 3 : min; fd < NFD; fd++)
    if (!fds[fd].used)
      return fd;
  errno = EMFILE;
  return -1;
}

/* Gives a new descriptor for the file of fd. */
int dup(int fd) {
  int to;
  if (bad(fd))
    return -1;
  to = free_fd(3);
  return to < 0 ? -1 : copy_fd(fd, to);
}

/* Makes descriptor to a copy of descriptor fd. */
int dup2(int fd, int to) {
  if (to < 3 || to >= NFD) {
    errno = EBADF;
    return -1;
  }
  return copy_fd(fd, to);
}

/* The fcntl commands for descriptors: F_DUPFD, F_GETFD, F_SETFD, F_GETFL
   and F_SETFL (O_APPEND and O_NONBLOCK; reads and writes never wait for
   long, so O_NONBLOCK changes nothing). */
int fcntl(int fd, int cmd, ...) {
  va_list ap;
  int arg, to;
  va_start(ap, cmd);
  arg = va_arg(ap, int);
  va_end(ap);
  if (fd >= 0 && fd < 3)
    return cmd == F_GETFL ? (fd == 0 ? O_RDONLY : O_WRONLY) : 0;
  if (bad(fd))
    return -1;
  switch (cmd) {
  case F_DUPFD:
    to = free_fd(arg);
    return to < 0 ? -1 : copy_fd(fd, to);
  case F_GETFD:
  case F_SETFD:
    return 0;
  case F_GETFL:
    return fds[fd].flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);
  case F_SETFL:
    fds[fd].flags = ((fds[fd].flags & ~(O_APPEND | O_NONBLOCK)) |
                            (arg & (O_APPEND | O_NONBLOCK)));
    return 0;
  default:
    errno = EINVAL;
    return -1;
  }
}

/* Writes the file's buffers to the disk. */
int fsync(int fd) {
  ParamBlockRec pb;
  OSErr err;
  if (fd >= 0 && fd < 3)
    return 0;
  if (bad(fd))
    return -1;
  if (fds[fd].ops)
    return 0;
  pb.ioParam.ioCompletion = 0;
  pb.ioParam.ioRefNum = fds[fd].ref;
  err = PBFlushFileSync(&pb);
  return err == noErr ? 0 : fail(err);
}

/* Reads up to n bytes from fd (the console for descriptor 0). */
ssize_t read(int fd, void *buf, size_t n) {
  long count = (long)n;
  OSErr err;
  if (fd == 0) {
    count = __mac_console_read(buf, n);
    if (count < 0)
      errno = EIO;
    return count;
  }
  if (bad(fd))
    return -1;
  if ((fds[fd].flags & O_ACCMODE) == O_WRONLY) {
    errno = EBADF;
    return -1;
  }
  if (fds[fd].ops)
    return fds[fd].ops->read(fds[fd].obj, buf, n,
                             (fds[fd].flags & O_NONBLOCK) != 0);
  err = FSRead(fds[fd].ref, &count, buf);
  if (err != noErr && err != eofErr)
    return fail(err);
  return count;
}

/* Writes n bytes to fd (the console for descriptors 1 and 2). */
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
  if (fds[fd].ops)
    return fds[fd].ops->write(fds[fd].obj, buf, n,
                              (fds[fd].flags & O_NONBLOCK) != 0);
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

/* Moves the mark of fd, and makes the file longer if the new mark is
   after its end. */
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
  if (fds[fd].ops) {
    errno = ESPIPE;
    return -1;
  }
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

/* Reads at offset without a change to the mark of fd. */
ssize_t pread(int fd, void *buf, size_t n, off_t offset) {
  off_t was = lseek(fd, 0, SEEK_CUR);
  ssize_t got;
  if (was < 0 || lseek(fd, offset, SEEK_SET) < 0)
    return -1;
  got = read(fd, buf, n);
  lseek(fd, was, SEEK_SET);
  return got;
}

/* Writes at offset without a change to the mark of fd. */
ssize_t pwrite(int fd, const void *buf, size_t n, off_t offset) {
  off_t was = lseek(fd, 0, SEEK_CUR);
  ssize_t put;
  if (was < 0 || lseek(fd, offset, SEEK_SET) < 0)
    return -1;
  put = write(fd, buf, n);
  lseek(fd, was, SEEK_SET);
  return put;
}

/* Gives the File Manager reference number of an open file descriptor,
   or 0 for the console or a descriptor that is not open. */
short __mac_fd_ref(int fd) {
  if (fd < 3 || fd >= NFD || !fds[fd].used)
    return 0;
  return fds[fd].ref;
}

/* Tells if fd is the console. */
int isatty(int fd) {
  if (fd >= 0 && fd < 3)
    return 1;
  errno = ENOTTY;
  return 0;
}

/* Reads into each buffer in turn, and stops at the first short read. */
ssize_t readv(int fd, const struct iovec *iov, int count) {
  ssize_t total = 0;
  int i;
  for (i = 0; i < count; i++) {
    ssize_t got = read(fd, iov[i].iov_base, iov[i].iov_len);
    if (got < 0)
      return total ? total : -1;
    total += got;
    if ((size_t)got < iov[i].iov_len)
      break;
  }
  return total;
}

/* Writes each buffer in turn, and stops at the first short write. */
ssize_t writev(int fd, const struct iovec *iov, int count) {
  ssize_t total = 0;
  int i;
  for (i = 0; i < count; i++) {
    ssize_t put = write(fd, iov[i].iov_base, iov[i].iov_len);
    if (put < 0)
      return total ? total : -1;
    total += put;
    if ((size_t)put < iov[i].iov_len)
      break;
  }
  return total;
}
