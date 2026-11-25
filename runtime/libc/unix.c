/* Pipes between programs on one Mac: AF_UNIX stream sockets and named
 * pipes (mkfifo), over the PPC Toolbox of System 7.
 *
 * A socket path names a PPC port of type "POSIX socket" on this Mac.
 * Only the last 32 characters of the path count, because a port name
 * has at most 32. bind opens the port; a connection is a PPC session.
 * A session gives each write to the other end's read as a block, and a
 * write ends when the other end has read it. shutdown(SHUT_WR) sends an
 * empty block of type 'EOF ', which the other end reads as the end.
 *
 * A named pipe is a file of type 'fifo'. open(O_RDONLY) opens the port
 * of its path and waits for a writer; open(O_WRONLY) waits for a reader
 * and connects to it. One writer at a time can connect.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <Events.h>
#include <Files.h>
#include <Gestalt.h>
#include <PPCToolbox.h>
#include "macos.h"
#include "socket.h"

/* The PPC Toolbox's results, which the headers do not name. */
enum {
  kNoPortErr = -903,
  kDestPortErr = -906,
  kPortNameExistsErr = -910,
  kUserRejectErr = -912,
  kPortClosedErr = -916,
  kSessClosedErr = -917
};

#define BLOCK 2048           /* the most bytes that one read takes */
#define EOF_BLOCK 'EOF '     /* the block type of shutdown(SHUT_WR) */

struct ux {
  PPCPortRec name;           /* the port's name: the socket's path */
  LocationNameRec here;      /* this Mac */
  PPCPortRefNum port;
  int has_port;
  PPCInformPBRec inform;     /* a listener's wait for a session */
  PPCPortRec from;           /* the port of a session that came */
  LocationNameRec fromLoc;
  int informing;
  PPCSessRefNum sess;
  int has_sess;
  PPCReadPBRec rd;           /* the read in progress */
  int reading, eof;
  char buf[BLOCK];
  long len, off;             /* bytes read and not given yet */
  char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  char peer[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

/* Starts the PPC Toolbox the first time. Gives 0, or -1 with errno. */
static int ppc_start(void) {
  static int state;  /* 0 not tried, 1 running, -1 not there */
  long attr;
  if (state == 0) {
    state = Gestalt(gestaltPPCToolboxAttr, &attr) == noErr && PPCInit() == noErr ? 1 : -1;
  }
  if (state < 0) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  return 0;
}

/* Waits for the end of the PPC call whose parameter block is pb, and
   gives its result. */
static OSErr wait_call(void *pb) {
  volatile OSErr *result = &((PPCOpenPBRec *)pb)->ioResult;
  while (*result > 0)
    __mac_wait();
  return *result;
}

/* Tells if the PPC call of pb has ended. */
static int ended(void *pb) { return ((PPCOpenPBRec *)pb)->ioResult <= 0; }

/* errno for a PPC Toolbox result. */
static int ppc_errno(OSErr err) {
  switch (err) {
  case kNoPortErr:
  case kDestPortErr:
  case kUserRejectErr: return ECONNREFUSED;
  case kPortNameExistsErr: return EADDRINUSE;
  case kPortClosedErr:
  case kSessClosedErr: return EPIPE;
  default: return EIO;
  }
}

/* Makes a port name from a path: its last 32 characters. */
static void port_name(PPCPortRec *r, const char *path) {
  size_t n = strlen(path);
  const char *from = n > 32 ? path + n - 32 : path;
  memset(r, 0, sizeof *r);
  r->nameScript = 0;  /* smRoman */
  r->name[0] = (unsigned char)strlen(from);
  memcpy(r->name + 1, from, r->name[0]);
  r->portKindsSelector = ppcByString;
  memcpy(r->u.portTypeStr, "\pPOSIX socket", 13);
}

/* Opens the port named by path. Gives 0, or -1 with errno. */
static int open_port(struct ux *u, const char *path) {
  PPCOpenPBRec pb;
  OSErr err;
  port_name(&u->name, path);
  memset(&u->here, 0, sizeof u->here);
  u->here.locationKindSelector = ppcNoLocation;
  memset(&pb, 0, sizeof pb);
  pb.serviceType = ppcServiceRealTime;
  pb.portName = &u->name;
  pb.locationName = &u->here;
  pb.networkVisible = 0;
  PPCOpenAsync(&pb);
  err = wait_call(&pb);
  if (err != noErr) {
    errno = ppc_errno(err);
    return -1;
  }
  u->port = pb.portRefNum;
  u->has_port = 1;
  return 0;
}

/* Gives the path of a sockaddr_un, or NULL with errno. */
static const char *path_of(const struct sockaddr *a, socklen_t len) {
  const struct sockaddr_un *un = (const struct sockaddr_un *)a;
  if (a->sa_family != AF_UNIX || len <= sizeof(sa_family_t) || !un->sun_path[0]) {
    errno = EINVAL;
    return 0;
  }
  return un->sun_path;
}

/* Opens the socket's port at the path of a. */
static int ux_bind(struct sock *k, const struct sockaddr *a, socklen_t len) {
  struct ux *u = k->impl;
  const char *path = path_of(a, len);
  if (!path)
    return -1;
  if (u->has_port) {
    errno = EINVAL;
    return -1;
  }
  if (open_port(u, path) < 0)
    return -1;
  strncpy(u->path, path, sizeof u->path - 1);
  return 0;
}

/* Starts the wait for a session on a listener's port. */
static void inform(struct ux *u) {
  memset(&u->inform, 0, sizeof u->inform);
  u->inform.portRefNum = u->port;
  u->inform.autoAccept = 1;
  u->inform.portName = &u->from;
  u->inform.locationName = &u->fromLoc;
  PPCInformAsync(&u->inform);
  u->informing = 1;
}

/* Makes a bound socket wait for connections. */
static int ux_listen(struct sock *k, int backlog) {
  struct ux *u = k->impl;
  (void)backlog;
  if (!u->has_port) {
    errno = EINVAL;
    return -1;
  }
  if (!u->informing)
    inform(u);
  return 0;
}

/* Gives a listener's next connection to c. */
static int ux_accept(struct sock *k, struct sock *c, int nonblock) {
  struct ux *u = k->impl, *cu = c->impl;
  OSErr err;
  for (;;) {
    while (!ended(&u->inform)) {
      if (nonblock) {
        errno = EAGAIN;
        return -1;
      }
      __mac_wait();
    }
    err = u->inform.ioResult;
    if (err == noErr)
      break;
    inform(u);
    if (err == kPortClosedErr) {
      errno = EBADF;
      return -1;
    }
  }
  cu->sess = u->inform.sessRefNum;
  cu->has_sess = 1;
  memcpy(cu->path, u->path, sizeof cu->path);
  memcpy(cu->peer, u->from.name + 1, u->from.name[0]);
  cu->peer[u->from.name[0]] = 0;
  c->connected = 1;
  inform(u);
  return 0;
}

/* Connects to the port that the path of a names. */
static int ux_connect(struct sock *k, const struct sockaddr *a, socklen_t len, int nonblock) {
  static unsigned count;
  struct ux *u = k->impl;
  const char *path = path_of(a, len);
  PPCStartPBRec pb;
  PPCPortRec target;
  OSErr err;
  (void)nonblock;
  if (!path)
    return -1;
  if (k->connected) {
    errno = EISCONN;
    return -1;
  }
  if (!u->has_port) {
    /* A port of our own to start the session from, with a name that no
       other port has. */
    char name[32];
    unsigned long t = TickCount();
    int i = 0;
    unsigned n = ++count;
    memcpy(name, "client ", 7);
    i = 7;
    while (t && i < 20) {
      name[i++] = (char)('a' + t % 26);
      t /= 26;
    }
    name[i++] = '-';
    do {
      name[i++] = (char)('0' + n % 10);
      n /= 10;
    } while (n && i < 31);
    name[i] = 0;
    if (open_port(u, name) < 0)
      return -1;
  }
  port_name(&target, path);
  memset(&pb, 0, sizeof pb);
  pb.portRefNum = u->port;
  pb.serviceType = ppcServiceRealTime;
  pb.portName = &target;
  pb.locationName = &u->here;
  PPCStartAsync(&pb);
  err = wait_call(&pb);
  if (err != noErr) {
    errno = ppc_errno(err);
    return -1;
  }
  u->sess = pb.sessRefNum;
  u->has_sess = 1;
  strncpy(u->peer, path, sizeof u->peer - 1);
  k->connected = 1;
  return 0;
}

/* Writes one block of len bytes of the given type to the session. */
static OSErr put_block(struct ux *u, const void *buf, long len, OSType type) {
  PPCWritePBRec pb;
  memset(&pb, 0, sizeof pb);
  pb.sessRefNum = u->sess;
  pb.bufferLength = len;
  pb.bufferPtr = (Ptr)buf;
  pb.more = 0;
  pb.blockCreator = 0x3F3F3F3FUL /* '????' */;
  pb.blockType = type;
  PPCWriteAsync(&pb);
  return wait_call(&pb);
}

/* Writes to the other end; ends when the other end has read it all. */
static long ux_send(struct sock *k, const void *buf, size_t n, int nonblock, const struct sockaddr *to,
                    socklen_t tolen) {
  struct ux *u = k->impl;
  size_t sent = 0;
  (void)nonblock;
  (void)to;
  (void)tolen;
  if (!k->connected || !u->has_sess) {
    errno = ENOTCONN;
    return -1;
  }
  if (k->wrshut) {
    errno = EPIPE;
    return -1;
  }
  while (sent < n) {
    long part = n - sent > 32768 ? 32768 : (long)(n - sent);
    OSErr err = put_block(u, (const char *)buf + sent, part, 'data');
    if (err != noErr) {
      if (sent)
        return (long)sent;
      errno = ppc_errno(err);
      return -1;
    }
    sent += (size_t)part;
  }
  return (long)sent;
}

/* Starts a read of the session, unless one is in progress, data is
   waiting or the other end has ended. */
static void start_read(struct ux *u) {
  if (u->reading || u->eof || u->off < u->len || !u->has_sess)
    return;
  memset(&u->rd, 0, sizeof u->rd);
  u->rd.sessRefNum = u->sess;
  u->rd.bufferLength = BLOCK;
  u->rd.bufferPtr = u->buf;
  PPCReadAsync(&u->rd);
  u->reading = 1;
}

/* Takes the result of a read that has ended. */
static void end_read(struct ux *u) {
  u->reading = 0;
  if (u->rd.ioResult != noErr || (u->rd.blockType == EOF_BLOCK && u->rd.actualLength == 0)) {
    u->eof = 1;
    return;
  }
  u->len = u->rd.actualLength;
  u->off = 0;
}

/* Takes the session of a named pipe's writer, when a reader opened
   with O_NONBLOCK before the writer came. */
static void adopt(struct sock *k) {
  struct ux *u = k->impl;
  if (!k->connected || u->has_sess || !u->informing || !ended(&u->inform))
    return;
  if (u->inform.ioResult == noErr) {
    u->informing = 0;
    u->sess = u->inform.sessRefNum;
    u->has_sess = 1;
  } else {
    inform(u);
  }
}

/* Reads what the other end wrote; 0 at its end. */
static long ux_recv(struct sock *k, void *buf, size_t n, int nonblock, int peek, struct sockaddr *from,
                    socklen_t *fromlen) {
  struct ux *u = k->impl;
  unsigned long end = k->rcvtimeo ? TickCount() + (k->rcvtimeo * 60 + 999) / 1000 : 0;
  long got;
  if (!k->connected) {
    errno = ENOTCONN;
    return -1;
  }
  while (u->off >= u->len) {
    if (u->eof || k->rdshut || n == 0)
      return 0;
    adopt(k);
    start_read(u);
    if (u->reading && ended(&u->rd)) {
      end_read(u);
      continue;
    }
    if (nonblock || (end && (long)(end - TickCount()) <= 0)) {
      errno = EAGAIN;
      return -1;
    }
    __mac_wait();
  }
  got = u->len - u->off;
  if ((size_t)got > n)
    got = (long)n;
  memcpy(buf, u->buf + u->off, (size_t)got);
  if (!peek)
    u->off += got;
  if (from && fromlen)
    k->proto->name(k, 1, from, fromlen);
  return got;
}

/* The poll events of an AF_UNIX socket. */
static short ux_ready(struct sock *k) {
  struct ux *u = k->impl;
  adopt(k);
  if (u->informing && !k->connected)
    return ended(&u->inform) ? POLLIN : 0;
  if (!k->connected)
    return POLLOUT | POLLHUP;
  if (u->off < u->len)
    return POLLIN | POLLOUT;
  start_read(u);
  if (u->reading && ended(&u->rd))
    end_read(u);
  if (u->off < u->len)
    return POLLIN | POLLOUT;
  if (u->eof)
    return POLLIN | POLLHUP;
  return k->wrshut ? 0 : POLLOUT;
}

/* The bytes that a read gives now. */
static long ux_nread(struct sock *k) {
  struct ux *u = k->impl;
  ux_ready(k);
  return u->len - u->off;
}

/* Ends reading or writing. The end of writing is an 'EOF ' block. */
static int ux_shutdown(struct sock *k, int how) {
  struct ux *u = k->impl;
  if (!k->connected) {
    errno = ENOTCONN;
    return -1;
  }
  if (how != SHUT_WR)
    k->rdshut = 1;
  if (how != SHUT_RD && !k->wrshut) {
    k->wrshut = 1;
    put_block(u, "", 0, EOF_BLOCK);
  }
  return 0;
}

/* Ends the session and closes the port. */
static void ux_close(struct sock *k) {
  struct ux *u = k->impl;
  if (u->has_sess) {
    PPCEndPBRec pb;
    memset(&pb, 0, sizeof pb);
    pb.sessRefNum = u->sess;
    PPCEndAsync(&pb);
    wait_call(&pb);
    while (u->reading && !ended(&u->rd))
      __mac_wait();
  }
  if (u->has_port) {
    PPCClosePBRec pb;
    memset(&pb, 0, sizeof pb);
    pb.portRefNum = u->port;
    PPCCloseAsync(&pb);
    wait_call(&pb);
    while (u->informing && !ended(&u->inform))
      __mac_wait();
  }
  free(u);
}

/* Gives the socket's path or the other end's. */
static int ux_name(struct sock *k, int peer, struct sockaddr *a, socklen_t *len) {
  struct ux *u = k->impl;
  struct sockaddr_un un;
  if (peer && !k->connected) {
    errno = ENOTCONN;
    return -1;
  }
  memset(&un, 0, sizeof un);
  un.sun_family = AF_UNIX;
  strncpy(un.sun_path, peer ? u->peer : u->path, sizeof un.sun_path - 1);
  memcpy(a, &un, *len < sizeof un ? *len : sizeof un);
  *len = sizeof un;
  return 0;
}

static const struct proto ux_proto = {ux_bind, ux_listen, ux_accept, ux_connect, ux_send, ux_recv,
                                      ux_ready, ux_nread, ux_shutdown, ux_close, ux_name};

/* Makes a new AF_UNIX socket, after it starts the PPC Toolbox. */
int __mac_unix_new(struct sock *k) {
  if (ppc_start() < 0)
    return -1;
  k->impl = calloc(1, sizeof(struct ux));
  if (!k->impl) {
    errno = ENOBUFS;
    return -1;
  }
  k->proto = &ux_proto;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Named pipes */

/* Reads from a named pipe. */
static long fifo_read(void *obj, void *buf, size_t n, int nonblock) {
  struct sock *k = obj;
  return ux_recv(k, buf, n, nonblock, 0, NULL, NULL);
}

/* Writes to a named pipe. */
static long fifo_write(void *obj, const void *buf, size_t n, int nonblock) {
  struct sock *k = obj;
  return ux_send(k, buf, n, nonblock, NULL, 0);
}

/* The poll events of a named pipe. */
static short fifo_ready(void *obj) { return ux_ready(obj); }

/* Closes a named pipe. */
static int fifo_close(void *obj) {
  ux_close(obj);
  free(obj);
  return 0;
}

/* The bytes that a read of a named pipe gives now. */
static long fifo_nread(void *obj) { return ux_nread(obj); }

static const struct __mac_fd_ops fifo_ops = {fifo_read, fifo_write, fifo_ready, fifo_close, fifo_nread,
                                             S_IFIFO};

/* Makes a named pipe: an empty file of type 'fifo'. */
int mkfifo(const char *path, mode_t mode) {
  Str255 name;
  OSErr err;
  (void)mode;
  if (__mac_path(path, name) < 0)
    return -1;
  err = Create(name, 0, 'ttxt', 'fifo');
  if (err != noErr) {
    errno = __mac_errno(err);
    return -1;
  }
  return 0;
}

/* Opens a named pipe (open() calls it for a file of type 'fifo'). A
   reader opens the port of the path and waits for a writer; a writer
   waits for a reader and connects to it. With O_NONBLOCK, a reader does
   not wait, and a writer fails with ENXIO when there is no reader. */
int __mac_fifo_open(const char *path, int flags) {
  struct sock *k;
  struct ux *u;
  int acc = flags & O_ACCMODE, fd;
  struct sockaddr_un un;
  if (acc == O_RDWR) {
    errno = EINVAL;
    return -1;
  }
  k = calloc(1, sizeof *k);
  if (!k) {
    errno = ENOMEM;
    return -1;
  }
  k->family = AF_UNIX;
  k->type = SOCK_STREAM;
  if (__mac_unix_new(k) < 0) {
    free(k);
    return -1;
  }
  u = k->impl;
  memset(&un, 0, sizeof un);
  un.sun_family = AF_UNIX;
  strncpy(un.sun_path, path, sizeof un.sun_path - 1);
  if (acc == O_RDONLY) {
    if (ux_bind(k, (struct sockaddr *)&un, sizeof un) < 0)
      goto fail;
    inform(u);
    if (!(flags & O_NONBLOCK)) {
      while (!ended(&u->inform))
        __mac_wait();
    }
    /* The writer's session, now or (O_NONBLOCK) when it comes. */
    while (ended(&u->inform)) {
      if (u->inform.ioResult == noErr) {
        u->informing = 0;
        u->sess = u->inform.sessRefNum;
        u->has_sess = 1;
        k->connected = 1;
        break;
      }
      inform(u);
      if (flags & O_NONBLOCK)
        break;
      while (!ended(&u->inform))
        __mac_wait();
    }
    if (!k->connected) {
      /* O_NONBLOCK with no writer yet: reads give nothing until one
         comes. */
      k->connected = 1;
      u->eof = 0;
    }
  } else {
    while (ux_connect(k, (struct sockaddr *)&un, sizeof un, 0) < 0) {
      if (errno != ECONNREFUSED)
        goto fail;
      if (flags & O_NONBLOCK) {
        errno = ENXIO;
        goto fail;
      }
      __mac_wait();
    }
  }
  fd = __mac_fd_new(&fifo_ops, k, acc | (flags & O_NONBLOCK));
  if (fd < 0)
    goto fail;
  return fd;
fail : {
  int e = errno;
  ux_close(k);
  free(k);
  errno = e;
  return -1;
}
}
