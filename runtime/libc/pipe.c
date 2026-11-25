/* Pipes inside one program: pipe and pipe2. A pipe is a buffer in the
 * heap. A read waits for data and a write waits for space; meanwhile
 * the other threads run (see __mac_wait). A pipe between two programs
 * is a named pipe or an AF_UNIX socket (see ppc.c).
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "macos.h"

#define PIPE_SIZE 4096

struct pipe {
  char buf[PIPE_SIZE];
  size_t head, len;     /* the first byte to read, and how many there are */
  int readers, writers; /* the ends that are open */
};

/* One end of a pipe: each end is the object of its own descriptors. */
struct end {
  struct pipe *p;
  int writer;
};

/* Reads up to n bytes; waits while the pipe is empty and a writer is
   open. Gives 0 at the end (no writer and no data). */
static long pipe_read(void *obj, void *buf, size_t n, int nonblock) {
  struct pipe *p = ((struct end *)obj)->p;
  size_t got = 0;
  while (p->len == 0) {
    if (p->writers == 0 || n == 0)
      return 0;
    if (nonblock) {
      errno = EAGAIN;
      return -1;
    }
    __mac_wait();
  }
  while (got < n && p->len) {
    size_t part = PIPE_SIZE - p->head;
    if (part > p->len)
      part = p->len;
    if (part > n - got)
      part = n - got;
    memcpy((char *)buf + got, p->buf + p->head, part);
    p->head = (p->head + part) % PIPE_SIZE;
    p->len -= part;
    got += part;
  }
  return (long)got;
}

/* Writes n bytes; waits for space while a reader is open. A write to a
   pipe with no reader raises SIGPIPE and fails with EPIPE. */
static long pipe_write(void *obj, const void *buf, size_t n, int nonblock) {
  struct pipe *p = ((struct end *)obj)->p;
  size_t put = 0;
  while (put < n) {
    size_t at, part;
    if (p->readers == 0) {
      if (put)
        return (long)put;
      raise(SIGPIPE);
      errno = EPIPE;
      return -1;
    }
    if (p->len == PIPE_SIZE) {
      if (nonblock) {
        if (put)
          return (long)put;
        errno = EAGAIN;
        return -1;
      }
      __mac_wait();
      continue;
    }
    at = (p->head + p->len) % PIPE_SIZE;
    /* The free bytes from at to the end of the buffer or to head. */
    part = at >= p->head ? PIPE_SIZE - at : p->head - at;
    if (part > n - put)
      part = n - put;
    memcpy(p->buf + at, (const char *)buf + put, part);
    p->len += part;
    put += part;
  }
  return (long)put;
}

/* The poll events of one end. */
static short pipe_ready(void *obj) {
  struct end *e = obj;
  struct pipe *p = e->p;
  if (e->writer)
    return p->readers == 0 ? POLLERR : p->len < PIPE_SIZE ? POLLOUT : 0;
  return (p->len ? POLLIN : 0) | (p->writers == 0 ? POLLHUP : 0);
}

/* Closes one end; the pipe goes when both ends are closed. */
static int pipe_close(void *obj) {
  struct end *e = obj;
  struct pipe *p = e->p;
  if (e->writer)
    p->writers--;
  else
    p->readers--;
  free(e);
  if (p->readers == 0 && p->writers == 0)
    free(p);
  return 0;
}

/* The bytes a read gives now. */
static long pipe_nread(void *obj) {
  struct end *e = obj;
  return e->writer ? 0 : (long)e->p->len;
}

static const struct __mac_fd_ops pipe_ops = {
    pipe_read, pipe_write, pipe_ready, pipe_close, pipe_nread, S_IFIFO};

/* Makes a pipe: fds[0] reads what fds[1] writes. flags may have
   O_NONBLOCK and O_CLOEXEC (which changes nothing). */
int pipe2(int fds[2], int flags) {
  struct pipe *p = calloc(1, sizeof *p);
  struct end *r = malloc(sizeof *r), *w = malloc(sizeof *w);
  int nb = flags & O_NONBLOCK;
  if (!p || !r || !w) {
    free(p);
    free(r);
    free(w);
    errno = ENOMEM;
    return -1;
  }
  r->p = w->p = p;
  r->writer = 0;
  w->writer = 1;
  p->readers = p->writers = 1;
  fds[0] = __mac_fd_new(&pipe_ops, r, O_RDONLY | nb);
  if (fds[0] < 0) {
    free(p);
    free(r);
    free(w);
    return -1;
  }
  fds[1] = __mac_fd_new(&pipe_ops, w, O_WRONLY | nb);
  if (fds[1] < 0) {
    close(fds[0]);
    p->writers = 0;
    free(w);
    return -1;
  }
  return 0;
}

/* Makes a pipe: fds[0] reads what fds[1] writes. */
int pipe(int fds[2]) { return pipe2(fds, 0); }
