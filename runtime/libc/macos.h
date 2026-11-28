/* The classic Mac OS layer under picolibc: what the C library's file,
 * console, memory and time functions call, written with this SDK's
 * Toolbox headers.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef MACOS_LIBC_H
#define MACOS_LIBC_H

#include <stddef.h>
#include <Files.h>

/* errno for a File Manager or Memory Manager result. */
int __mac_errno(OSErr err);

/* A POSIX or Mac path as a Pascal string: "a/b" is ":a:b", "/Disk/a" is
 * "Disk:a", and a name without "/" is used as it is (a Mac path). Returns
 * 0, or -1 with errno set when the path is too long. */
int __mac_path(const char *path, Str255 out);

/* The File Manager reference number of file descriptor fd, or 0 when fd
   is not an open file (the console, or a closed descriptor). */
short __mac_fd_ref(int fd);

/* The POSIX path of folder dir on volume vol (0 and 0: the current
   folder), in buf or in a new block when buf is NULL. Gives NULL with
   errno set on an error. */
char *__mac_folder_path(short vol, long dir, char *buf, size_t size);

/* Seconds east of UTC of the Mac's clock. */
long __mac_gmt_delta(void);

/* The catalog information of path (a POSIX or Mac path; "" and "." are
   the current folder). The name buffer must stay valid while pb is used.
   Returns noErr or the File Manager's error. */
OSErr __mac_cat_info(const char *path, CInfoPBRec *pb, Str255 name);

/* The Mac's file date (local seconds since 1904) as a time_t, and back. */
long long __mac_date_to_time(unsigned long date);
unsigned long __mac_time_to_date(long long t);

/* Console output (stdout and stderr). */
void __mac_console_write(const void *buf, size_t n);
void __mac_console_close(void);

/* Console input (stdin): up to n bytes, 0 at end of file, or -1. */
long __mac_console_read(void *buf, size_t n);

/* A descriptor that is not a file: a pipe or a socket. The functions
   get the object that __mac_fd_new was given. read and write give the
   count, or -1 with errno set; with nonblock set they give -1 with
   EAGAIN instead of waiting. ready gives the poll events (POLLIN,
   POLLOUT, POLLHUP, POLLERR) that are true now. close is called when the
   last descriptor of the object closes. */
struct __mac_fd_ops {
  long (*read)(void *obj, void *buf, size_t n, int nonblock);
  long (*write)(void *obj, const void *buf, size_t n, int nonblock);
  short (*ready)(void *obj);
  int (*close)(void *obj);
  long (*nread)(void *obj);    /* the bytes a read gives now (FIONREAD) */
  unsigned mode;               /* S_IFIFO or S_IFSOCK, for fstat */
};

/* Gives a new descriptor (3 or more) for obj, with the open() flags
   (O_RDONLY, O_WRONLY or O_RDWR, and O_NONBLOCK), or -1 with errno
   set. */
int __mac_fd_new(const struct __mac_fd_ops *ops, void *obj, int flags);

/* The object of descriptor fd when its functions are ops (NULL for
   any), or NULL with errno set: EBADF when fd is not open, ENOTSOCK
   when it has other functions. *flags gets its open() flags when flags
   is not NULL. */
void *__mac_fd_obj(int fd, const struct __mac_fd_ops *ops, int *flags);

/* The functions of descriptor fd, or NULL when it is a file, the
   console or not open. */
const struct __mac_fd_ops *__mac_fd_ops(int fd);

/* Opens a named pipe (unix.c). */
int __mac_fifo_open(const char *path, int flags);

/* Lets the other threads and the other programs run for a moment: the
   way to wait for a pipe, a socket or a driver. */
void __mac_wait(void);

/* Calls fn, a function of the MPW C calling convention (68k), with
   nargs 4-byte arguments, and keeps the registers that clang keeps.
   Gives D0. In aslm.c. */
long __mac_mpw_call(const void *fn, int nargs, const long *args);

#endif
