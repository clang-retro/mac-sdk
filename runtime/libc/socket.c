/* Sockets: the BSD socket calls, and AF_INET sockets over MacTCP.
 *
 * MacTCP is the .IPP driver of System 7. Open Transport gives the same
 * driver, so the calls work up to Mac OS 9. Each call is a control call
 * that this file makes asynchronously; while it waits, the other
 * threads and programs run (__mac_wait).
 *
 * A MacTCP TCP stream holds one connection. A listening socket keeps
 * some streams waiting on its port (up to the backlog, at most 4);
 * accept takes a stream that has a connection and puts a new stream in
 * its place. A UDP socket is one UDP stream with one read in progress.
 *
 * AF_UNIX sockets, between programs on one Mac, are in unix.c.
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
#include <sys/time.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <Devices.h>
#include <Events.h>
#include <Multiverse.h>
#include "macos.h"
#include "socket.h"

#define TCP_BUFFER 16384      /* a TCP stream's receive buffer */
#define UDP_BUFFER 8192       /* a UDP stream's receive buffer */
#define MAX_PENDING 4         /* streams that wait for connections */
#define SEGMENT 32768         /* the most bytes one TCPSend sends */

/* A TCP stream and its parameter block, for a call in progress. */
struct stream {
  StreamPtr s;
  char *buf;
  TCPiopb pb;
};

struct inet {
  struct stream conn;               /* the connection, or none (s 0) */
  struct stream pend[MAX_PENDING];  /* a listener's waiting streams */
  int npend;
  TCPiopb closepb;                  /* TCPClose after shutdown */
  int closing;
  char in[1024];                    /* bytes taken from MacTCP but not read */
  size_t inlen;
  UDPiopb upb;              /* the UDP read in progress */
  int reading;
  char *ubuf;
  StreamPtr us;
};

/* ------------------------------------------------------------------ */
/* The .IPP driver */

static short ipp;  /* its reference number, 0 before it is opened */

/* Opens the .IPP driver the first time. Gives 0, or -1 with errno. */
static int driver(void) {
  OSErr err;
  if (ipp)
    return 0;
  err = OpenDriver((ConstStringPtr) "\p.IPP", &ipp);
  if (err != noErr) {
    ipp = 0;
    errno = ENETDOWN;
    return -1;
  }
  return 0;
}

/* errno for a MacTCP result. */
static int tcp_errno(OSErr err) {
  switch (err) {
  case openFailed: return ECONNREFUSED;
  case connectionTerminated: return ECONNRESET;
  case connectionDoesntExist: return ENOTCONN;
  case connectionClosing: return EPIPE;
  case connectionExists:
  case duplicateSocket: return EADDRINUSE;
  case commandTimeout: return ETIMEDOUT;
  case insufficientResources:
  case outOfMemory: return ENOBUFS;
  case invalidLength: return EINVAL;
  case ipBadAddr: return EADDRNOTAVAIL;
  case ipBadLapErr:
  case ipBadCnfgErr:
  case ipNoCnfgErr:
  case ipLoadErr: return ENETDOWN;
  default: return EIO;
  }
}

/* Starts a control call of the driver. */
static void start(void *pb, short code) {
  TCPiopb *p = pb;
  p->ioCompletion = 0;
  p->ioCRefNum = ipp;
  p->csCode = code;
  p->ioResult = inProgress;
  PBControlAsync((ParmBlkPtr)p);
}

/* Tells if the call of pb has ended. */
static int done(const void *pb) { return ((const TCPiopb *)pb)->ioResult <= 0; }

/* Makes a control call and waits for its end. Gives its result. */
static OSErr call(void *pb, short code) {
  start(pb, code);
  while (!done(pb))
    __mac_wait();
  return ((TCPiopb *)pb)->ioResult;
}

/* Gives our IP address, or 0. */
in_addr_t __mac_ip_address(void) {
  GetAddrParamBlock pb;
  if (driver() < 0)
    return 0;
  memset(&pb, 0, sizeof pb);
  if (call(&pb, ipctlGetAddr) != noErr)
    return 0;
  return pb.ourAddress;
}

/* ------------------------------------------------------------------ */
/* TCP streams */

/* Makes a TCP stream. Gives 0, or -1 with errno. */
static int new_stream(struct stream *t) {
  OSErr err;
  memset(t, 0, sizeof *t);
  t->buf = malloc(TCP_BUFFER);
  if (!t->buf) {
    errno = ENOBUFS;
    return -1;
  }
  t->pb.csParam.create.rcvBuff = t->buf;
  t->pb.csParam.create.rcvBuffLen = TCP_BUFFER;
  t->pb.csParam.create.notifyProc = 0;
  err = call(&t->pb, TCPCreate);
  if (err != noErr) {
    free(t->buf);
    t->buf = 0;
    errno = tcp_errno(err);
    return -1;
  }
  t->s = t->pb.tcpStream;
  return 0;
}

/* Ends a TCP stream and its connection at once. */
static void release(struct stream *t) {
  TCPiopb pb;
  if (!t->s)
    return;
  if (!done(&t->pb)) {
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = t->s;
    call(&pb, TCPAbort);
    while (!done(&t->pb))
      __mac_wait();
  }
  memset(&pb, 0, sizeof pb);
  pb.tcpStream = t->s;
  call(&pb, TCPRelease);
  free(t->buf);
  t->s = 0;
  t->buf = 0;
}

/* Starts a passive open on a stream: it waits for a connection to
   host and port. */
static void passive(struct stream *t, ip_addr host, unsigned short port) {
  memset(&t->pb.csParam, 0, sizeof t->pb.csParam);
  t->pb.tcpStream = t->s;
  t->pb.csParam.open.localHost = host;
  t->pb.csParam.open.localPort = port;
  start(&t->pb, TCPPassiveOpen);
}

/* Gets the status of a stream's connection. Gives the MacTCP result. */
static OSErr status(StreamPtr s, TCPStatusPB *st) {
  TCPiopb pb;
  OSErr err;
  memset(&pb, 0, sizeof pb);
  pb.tcpStream = s;
  err = call(&pb, TCPStatus);
  *st = pb.csParam.status;
  return err;
}

/* ------------------------------------------------------------------ */
/* AF_INET */

static int in_addr_of(const struct sockaddr *a, socklen_t len, ip_addr *host, unsigned short *port) {
  const struct sockaddr_in *in = (const struct sockaddr_in *)a;
  if (len < sizeof *in || a->sa_family != AF_INET) {
    errno = a && a->sa_family == AF_INET6 ? EAFNOSUPPORT : EINVAL;
    return -1;
  }
  *host = in->sin_addr.s_addr;
  *port = in->sin_port;
  return 0;
}

static int udp_open(struct sock *k);

/* The TCP sockets of this program that have a port: MacTCP finds a port
   in use only when it opens a stream, so bind looks here first. */
#define MAX_BOUND 32
static struct sock *bound[MAX_BOUND];

/* Tells if another TCP socket of this program has the port of k on an
   address that overlaps, and neither socket permits reuse. */
static int port_in_use(struct sock *k) {
  int i;
  for (i = 0; i < MAX_BOUND; i++) {
    struct sock *o = bound[i];
    if (!o || o == k || o->port != k->port)
      continue;
    if (o->host != k->host && o->host != INADDR_ANY && k->host != INADDR_ANY)
      continue;
    if (!(o->reuse && k->reuse) || o->listening)
      return 1;
  }
  return 0;
}

/* Adds k to the TCP sockets that have a port, or removes it. */
static void set_bound(struct sock *k, int on) {
  int i, free_slot = -1;
  for (i = 0; i < MAX_BOUND; i++) {
    if (bound[i] == k) {
      if (!on)
        bound[i] = NULL;
      return;
    }
    if (!bound[i] && free_slot < 0)
      free_slot = i;
  }
  if (on && free_slot >= 0)
    bound[free_slot] = k;
}

/* Keeps the address for the socket. MacTCP takes a TCP stream's port
   when it opens a connection; a UDP socket gets its stream (and port)
   now. A TCP port that another socket of this program has fails with
   EADDRINUSE, and port 0 gets a free port from 49152 to 65535 now. */
static int inet_bind(struct sock *k, const struct sockaddr *a, socklen_t len) {
  ip_addr host;
  unsigned short port;
  if (in_addr_of(a, len, &host, &port) < 0)
    return -1;
  k->host = host;
  k->port = port;
  if (k->type == SOCK_DGRAM)
    return udp_open(k);
  if (!port) {
    /* Choose the port now, so that getsockname gives it before listen. */
    static unsigned short next;
    int tries;
    if (!next)
      next = (unsigned short)(49152 + TickCount() % 8192);
    for (tries = 0; tries < 16384; tries++) {
      k->port = htons(next);
      next = next >= 65535 ? 49152 : next + 1;
      if (!port_in_use(k))
        break;
    }
  } else if (port_in_use(k)) {
    k->port = 0;
    errno = EADDRINUSE;
    return -1;
  }
  set_bound(k, 1);
  return 0;
}

/* Makes a UDP socket's stream the first time. */
static int udp_open(struct sock *k) {
  struct inet *n = k->impl;
  UDPiopb pb;
  OSErr err;
  if (n->us)
    return 0;
  n->ubuf = malloc(UDP_BUFFER);
  if (!n->ubuf) {
    errno = ENOBUFS;
    return -1;
  }
  memset(&pb, 0, sizeof pb);
  pb.csParam.create.rcvBuff = n->ubuf;
  pb.csParam.create.rcvBuffLen = UDP_BUFFER;
  pb.csParam.create.localPort = k->port;
  err = call(&pb, udpCreate);
  if (err != noErr) {
    free(n->ubuf);
    n->ubuf = 0;
    errno = tcp_errno(err);
    return -1;
  }
  n->us = pb.udpStream;
  k->port = pb.csParam.create.localPort;
  return 0;
}

/* Starts a UDP read, unless one is in progress or has data. */
static void udp_read(struct inet *n) {
  if (n->reading)
    return;
  memset(&n->upb, 0, sizeof n->upb);
  n->upb.udpStream = n->us;
  n->upb.csParam.receive.timeOut = 0;  /* no time-out */
  start(&n->upb, udpRead);
  n->reading = 1;
}

/* Starts to wait for connections: opens up to "backlog" passive TCP streams
   on the bound port. */
static int inet_listen(struct sock *k, int backlog) {
  struct inet *n = k->impl;
  int i, want;
  if (k->type != SOCK_STREAM) {
    errno = EOPNOTSUPP;
    return -1;
  }
  if (n->npend)
    return 0;
  want = backlog < 1 ? 1 : backlog > MAX_PENDING ? MAX_PENDING : backlog;
  for (i = 0; i < want; i++) {
    if (new_stream(&n->pend[i]) < 0)
      break;
    passive(&n->pend[i], k->host, k->port);
    n->npend++;
    if (k->port == 0) {
      /* MacTCP chose the port: the next streams wait on it too. */
      TCPStatusPB st;
      if (status(n->pend[i].s, &st) == noErr)
        k->port = st.localPort;
    }
  }
  if (!n->npend)
    return -1;
  if (k->port)
    set_bound(k, 1);
  return 0;
}

/* Gives a waiting stream whose passive open has ended, or -1. */
static int accepted(struct inet *n) {
  int i;
  for (i = 0; i < n->npend; i++)
    if (done(&n->pend[i].pb))
      return i;
  return -1;
}

/* Gives the first stream that has a connection to the new socket "c", and
   opens a new passive stream in its place. */
static int inet_accept(struct sock *k, struct sock *c, int nonblock) {
  struct inet *n = k->impl, *cn = c->impl;
  struct stream *t;
  int i;
  OSErr err;
  for (;;) {
    while ((i = accepted(n)) < 0) {
      if (nonblock) {
        errno = EAGAIN;
        return -1;
      }
      __mac_wait();
    }
    t = &n->pend[i];
    err = t->pb.ioResult;
    if (err == noErr)
      break;
    /* The open failed (a time-out or an abort): wait again. */
    passive(t, k->host, k->port);
  }
  cn->conn = *t;
  c->peer_host = t->pb.csParam.open.remoteHost;
  c->peer_port = t->pb.csParam.open.remotePort;
  c->host = t->pb.csParam.open.localHost;
  c->port = t->pb.csParam.open.localPort;
  c->connected = 1;
  if (new_stream(t) == 0)
    passive(t, k->host, k->port);
  else
    n->pend[i] = n->pend[--n->npend];
  return 0;
}

/* Opens a TCP connection (active open) to the address, or sets the peer
   address of a UDP socket. */
static int inet_connect(struct sock *k, const struct sockaddr *a, socklen_t len, int nonblock) {
  struct inet *n = k->impl;
  ip_addr host;
  unsigned short port;
  if (in_addr_of(a, len, &host, &port) < 0)
    return -1;
  if (k->type == SOCK_DGRAM) {
    k->peer_host = host;
    k->peer_port = port;
    k->connected = 1;
    return udp_open(k);
  }
  if (k->connected) {
    errno = EISCONN;
    return -1;
  }
  if (k->connecting) {
    errno = EALREADY;
    return -1;
  }
  if (new_stream(&n->conn) < 0)
    return -1;
  memset(&n->conn.pb.csParam, 0, sizeof n->conn.pb.csParam);
  n->conn.pb.tcpStream = n->conn.s;
  n->conn.pb.csParam.open.remoteHost = host;
  n->conn.pb.csParam.open.remotePort = port;
  n->conn.pb.csParam.open.localPort = k->port;
  n->conn.pb.csParam.open.ulpTimeoutValue = 30;
  n->conn.pb.csParam.open.ulpTimeoutAction = 1;   /* abort */
  n->conn.pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
  start(&n->conn.pb, TCPActiveOpen);
  k->peer_host = host;
  k->peer_port = port;
  k->connecting = 1;
  if (nonblock) {
    errno = EINPROGRESS;
    return -1;
  }
  while (!done(&n->conn.pb))
    __mac_wait();
  return __mac_sock_connected(k);
}

/* Ends a connect in progress whose call has ended: gives 0, or -1 with
   errno and the socket's error set. */
int __mac_sock_connected(struct sock *k) {
  struct inet *n = k->impl;
  OSErr err = n->conn.pb.ioResult;
  k->connecting = 0;
  if (err != noErr) {
    release(&n->conn);
    k->error = errno = tcp_errno(err);
    return -1;
  }
  k->host = n->conn.pb.csParam.open.localHost;
  k->port = n->conn.pb.csParam.open.localPort;
  k->connected = 1;
  return 0;
}

/* Sends bytes on a TCP connection, or one datagram from a UDP socket. */
static long inet_send(struct sock *k, const void *buf, size_t len, int nonblock, const struct sockaddr *to,
                      socklen_t tolen) {
  struct inet *n = k->impl;
  size_t sent = 0;
  (void)nonblock;
  if (k->type == SOCK_DGRAM) {
    UDPiopb pb;
    wdsEntry wds[2];
    ip_addr host = k->peer_host;
    unsigned short port = k->peer_port;
    OSErr err;
    if (to && in_addr_of(to, tolen, &host, &port) < 0)
      return -1;
    if (!to && !k->connected) {
      errno = EDESTADDRREQ;
      return -1;
    }
    if (len > 65507) {
      errno = EMSGSIZE;
      return -1;
    }
    if (udp_open(k) < 0)
      return -1;
    wds[0].length = (uint16_t)len;
    wds[0].ptr = (Ptr)buf;
    wds[1].length = 0;
    memset(&pb, 0, sizeof pb);
    pb.udpStream = n->us;
    pb.csParam.send.remoteHost = host;
    pb.csParam.send.remotePort = port;
    pb.csParam.send.wdsPtr = (Ptr)wds;
    pb.csParam.send.checkSum = 1;
    err = call(&pb, udpWrite);
    if (err != noErr) {
      errno = tcp_errno(err);
      return -1;
    }
    return (long)len;
  }
  if (!k->connected) {
    errno = k->connecting ? EAGAIN : ENOTCONN;
    return -1;
  }
  if (k->wrshut) {
    errno = EPIPE;
    return -1;
  }
  while (sent < len) {
    TCPiopb pb;
    wdsEntry wds[2];
    size_t part = len - sent > SEGMENT ? SEGMENT : len - sent;
    OSErr err;
    wds[0].length = (uint16_t)part;
    wds[0].ptr = (Ptr)buf + sent;
    wds[1].length = 0;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = n->conn.s;
    pb.csParam.send.wdsPtr = (Ptr)wds;
    pb.csParam.send.pushFlag = 1;
    err = call(&pb, TCPSend);
    if (err != noErr) {
      if (sent)
        return (long)sent;
      errno = tcp_errno(err);
      if (errno == ENOTCONN || errno == ECONNRESET)
        errno = EPIPE;
      return -1;
    }
    sent += part;
  }
  return (long)sent;
}

/* Tells if a TCP connection gives no more data: the other end closed
   it, or it is gone. */
static int at_end(TCPStatusPB *st, OSErr err) {
  if (err != noErr)
    return 1;
  switch (st->connectionState) {
  case TCPSEstablished:
  case TCPSFinWait1:
  case TCPSFinWait2:
  case TCPSSynReceived:
  case TCPSSynSent:
    return 0;
  default:
    return 1;
  }
}

/* Takes up to n bytes that MacTCP has for a connection into n->in.
   Gives the count, 0 at the end, or -1 (EAGAIN) when there are none. */
static long take_data(struct inet *n) {
  TCPStatusPB st;
  TCPiopb pb;
  OSErr err = status(n->conn.s, &st);
  size_t want = sizeof n->in - n->inlen;
  if (err == noErr && st.amtUnreadData) {
    if (want > st.amtUnreadData)
      want = st.amtUnreadData;
    memset(&pb, 0, sizeof pb);
    pb.tcpStream = n->conn.s;
    pb.csParam.receive.rcvBuff = n->in + n->inlen;
    pb.csParam.receive.rcvBuffLen = (uint16_t)want;
    pb.csParam.receive.commandTimeoutValue = 1;
    err = call(&pb, TCPRcv);
    if (err == noErr) {
      n->inlen += pb.csParam.receive.rcvBuffLen;
      return pb.csParam.receive.rcvBuffLen;
    }
  }
  if (at_end(&st, err))
    return 0;
  errno = EAGAIN;
  return -1;
}

/* Receives bytes from a TCP connection, or one datagram on a UDP socket. */
static long inet_recv(struct sock *k, void *buf, size_t len, int nonblock, int peek, struct sockaddr *from,
                      socklen_t *fromlen) {
  struct inet *n = k->impl;
  unsigned long end = k->rcvtimeo ? TickCount() + (k->rcvtimeo * 60 + 999) / 1000 : 0;
  if (k->type == SOCK_DGRAM) {
    size_t got;
    if (udp_open(k) < 0)
      return -1;
    udp_read(n);
    while (!done(&n->upb)) {
      if (nonblock || (end && (long)(end - TickCount()) <= 0)) {
        errno = EAGAIN;
        return -1;
      }
      __mac_wait();
    }
    if (n->upb.ioResult != noErr) {
      n->reading = 0;
      errno = tcp_errno(n->upb.ioResult);
      return -1;
    }
    got = n->upb.csParam.receive.rcvBuffLen;
    if (got > len)
      got = len;
    memcpy(buf, n->upb.csParam.receive.rcvBuff, got);
    if (from && fromlen) {
      struct sockaddr_in in;
      memset(&in, 0, sizeof in);
      in.sin_family = AF_INET;
      in.sin_addr.s_addr = n->upb.csParam.receive.remoteHost;
      in.sin_port = n->upb.csParam.receive.remotePort;
      memcpy(from, &in, *fromlen < sizeof in ? *fromlen : sizeof in);
      *fromlen = sizeof in;
    }
    if (!peek) {
      UDPiopb pb;
      memset(&pb, 0, sizeof pb);
      pb.udpStream = n->us;
      pb.csParam.receive.rcvBuff = n->upb.csParam.receive.rcvBuff;
      call(&pb, udpBfrReturn);
      n->reading = 0;
      udp_read(n);
    }
    return (long)got;
  }
  if (!k->connected) {
    errno = ENOTCONN;
    return -1;
  }
  if (k->rdshut || len == 0)
    return 0;
  while (n->inlen == 0) {
    long got = take_data(n);
    if (got == 0)
      return 0;
    if (got > 0)
      break;
    if (nonblock || (end && (long)(end - TickCount()) <= 0)) {
      errno = EAGAIN;
      return -1;
    }
    __mac_wait();
  }
  if (len > n->inlen)
    len = n->inlen;
  memcpy(buf, n->in, len);
  if (!peek) {
    memmove(n->in, n->in + len, n->inlen - len);
    n->inlen -= len;
  }
  if (from && fromlen)
    __mac_sock_name(k, 1, from, fromlen);
  return (long)len;
}

/* Gives the poll events that are ready on an IP socket. */
static short inet_ready(struct sock *k) {
  struct inet *n = k->impl;
  TCPStatusPB st;
  OSErr err;
  if (k->type == SOCK_DGRAM) {
    short r = POLLOUT;
    if (n->us) {
      udp_read(n);
      if (done(&n->upb))
        r |= POLLIN;
    }
    return r;
  }
  if (n->npend)
    return accepted(n) >= 0 ? POLLIN : 0;
  if (k->connecting) {
    if (!done(&n->conn.pb))
      return 0;
    __mac_sock_connected(k);
    return k->error ? POLLOUT | POLLERR : POLLOUT;
  }
  if (!k->connected)
    return k->error ? POLLERR : POLLOUT | POLLHUP;
  if (n->inlen || k->rdshut)
    return POLLIN | (k->wrshut ? 0 : POLLOUT);
  err = status(n->conn.s, &st);
  if (err != noErr)
    return POLLIN | POLLHUP;
  if (st.amtUnreadData)
    return POLLIN | POLLOUT;
  if (at_end(&st, err))
    return POLLIN | POLLOUT | (st.connectionState == TCPSClosed ? POLLHUP : 0);
  return k->wrshut ? 0 : POLLOUT;
}

/* Gives the number of bytes that a read can get now. */
static long inet_nread(struct sock *k) {
  struct inet *n = k->impl;
  TCPStatusPB st;
  if (k->type == SOCK_DGRAM) {
    if (n->us && n->reading && done(&n->upb) && n->upb.ioResult == noErr)
      return n->upb.csParam.receive.rcvBuffLen;
    return 0;
  }
  if (!k->connected)
    return 0;
  if (status(n->conn.s, &st) != noErr)
    return (long)n->inlen;
  return (long)(n->inlen + st.amtUnreadData);
}

/* Ends the sending side of a TCP connection (TCPClose), or stops reads. */
static int inet_shutdown(struct sock *k, int how) {
  struct inet *n = k->impl;
  if (k->type == SOCK_STREAM && !k->connected) {
    errno = ENOTCONN;
    return -1;
  }
  if (how == SHUT_RD || how == SHUT_RDWR)
    k->rdshut = 1;
  if ((how == SHUT_WR || how == SHUT_RDWR) && !k->wrshut && k->type == SOCK_STREAM) {
    k->wrshut = 1;
    memset(&n->closepb, 0, sizeof n->closepb);
    n->closepb.tcpStream = n->conn.s;
    start(&n->closepb, TCPClose);
    n->closing = 1;
  }
  return 0;
}

/* Releases the MacTCP streams of an IP socket. */
static void inet_close(struct sock *k) {
  struct inet *n = k->impl;
  int i;
  set_bound(k, 0);
  if (k->type == SOCK_DGRAM) {
    if (n->us) {
      UDPiopb pb;
      memset(&pb, 0, sizeof pb);
      pb.udpStream = n->us;
      call(&pb, udpRelease);
      free(n->ubuf);
    }
  } else {
    for (i = 0; i < n->npend; i++)
      release(&n->pend[i]);
    if (n->conn.s) {
      if (k->connected && !k->wrshut)
        inet_shutdown(k, SHUT_WR);
      if (n->closing) {
        /* Let MacTCP send what is left, for up to 10 seconds. */
        unsigned long end = TickCount() + 600;
        while (!done(&n->closepb) && (long)(end - TickCount()) > 0)
          __mac_wait();
      }
      release(&n->conn);
    }
  }
  free(n);
}

/* Gives the local or the peer address of an IP socket. */
static int inet_name(struct sock *k, int peer, struct sockaddr *a, socklen_t *len) {
  struct sockaddr_in in;
  struct inet *n = k->impl;
  memset(&in, 0, sizeof in);
  in.sin_family = AF_INET;
  if (peer) {
    if (!k->connected) {
      errno = ENOTCONN;
      return -1;
    }
    in.sin_addr.s_addr = k->peer_host;
    in.sin_port = k->peer_port;
  } else {
    if (k->type == SOCK_STREAM && k->connected && n->conn.s) {
      TCPStatusPB st;
      if (status(n->conn.s, &st) == noErr) {
        k->host = st.localHost;
        k->port = st.localPort;
      }
    }
    in.sin_addr.s_addr = k->host;
    in.sin_port = k->port;
  }
  memcpy(a, &in, *len < sizeof in ? *len : sizeof in);
  *len = sizeof in;
  return 0;
}

static const struct proto inet_proto = {
    inet_bind, inet_listen, inet_accept, inet_connect, inet_send,
    inet_recv, inet_ready, inet_nread, inet_shutdown, inet_close, inet_name};

/* Makes the AF_INET part of a socket. */
static int inet_new(struct sock *k) {
  if (driver() < 0)
    return -1;
  k->impl = calloc(1, sizeof(struct inet));
  if (!k->impl) {
    errno = ENOBUFS;
    return -1;
  }
  k->proto = &inet_proto;
  return 0;
}

/* ------------------------------------------------------------------ */
/* The descriptors */

/* Reads from a socket descriptor (read). */
static long sock_read(void *obj, void *buf, size_t n, int nonblock) {
  struct sock *k = obj;
  return k->proto->recv(k, buf, n, nonblock, 0, NULL, NULL);
}

/* Writes to a socket descriptor (write). */
static long sock_write(void *obj, const void *buf, size_t n, int nonblock) {
  struct sock *k = obj;
  return k->proto->send(k, buf, n, nonblock, NULL, 0);
}

/* Gives the poll events of a socket descriptor. */
static short sock_ready(void *obj) {
  struct sock *k = obj;
  return k->proto->ready(k);
}

/* Closes a socket descriptor and releases the socket. */
static int sock_close(void *obj) {
  struct sock *k = obj;
  k->proto->close(k);
  free(k);
  return 0;
}

/* Gives the bytes that a socket descriptor can read now (FIONREAD). */
static long sock_nread(void *obj) {
  struct sock *k = obj;
  return k->proto->nread(k);
}

static const struct __mac_fd_ops sock_ops = {sock_read, sock_write, sock_ready, sock_close, sock_nread,
                                             S_IFSOCK};

/* Gives the socket of fd, or NULL with errno set (ENOTSOCK or EBADF). */
static struct sock *sock_of(int fd, int *nonblock) {
  int flags;
  struct sock *k = __mac_fd_obj(fd, &sock_ops, &flags);
  if (k && nonblock)
    *nonblock = (flags & O_NONBLOCK) != 0;
  return k;
}

/* Gives a descriptor for a new socket, or -1. */
static int new_fd(struct sock *k, int nonblock) {
  int fd = __mac_fd_new(&sock_ops, k, O_RDWR | (nonblock ? O_NONBLOCK : 0));
  if (fd < 0) {
    k->proto->close(k);
    free(k);
  }
  return fd;
}

/* Makes a socket: AF_INET over MacTCP, or AF_UNIX over the PPC Toolbox. */
int socket(int domain, int type, int protocol) {
  struct sock *k;
  int base = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
  int r;
  if (domain != AF_INET && domain != AF_UNIX) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  if (base != SOCK_STREAM && (base != SOCK_DGRAM || domain != AF_INET)) {
    errno = EPROTONOSUPPORT;
    return -1;
  }
  if (protocol && protocol != (base == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP)) {
    errno = EPROTONOSUPPORT;
    return -1;
  }
  k = calloc(1, sizeof *k);
  if (!k) {
    errno = ENOBUFS;
    return -1;
  }
  k->family = domain;
  k->type = base;
  r = domain == AF_INET ? inet_new(k) : __mac_unix_new(k);
  if (r < 0) {
    free(k);
    return -1;
  }
  return new_fd(k, (type & SOCK_NONBLOCK) != 0);
}

/* Not available: fails with EOPNOTSUPP. */
int socketpair(int domain, int type, int protocol, int sv[2]) {
  (void)domain;
  (void)type;
  (void)protocol;
  (void)sv;
  errno = EOPNOTSUPP;
  return -1;
}

/* Gives an address to a socket. */
int bind(int fd, const struct sockaddr *a, socklen_t len) {
  struct sock *k = sock_of(fd, NULL);
  if (!k)
    return -1;
  if (!a) {
    errno = EINVAL;
    return -1;
  }
  return k->proto->bind(k, a, len);
}

/* Starts to wait for connections on a socket. */
int listen(int fd, int backlog) {
  struct sock *k = sock_of(fd, NULL);
  if (!k)
    return -1;
  if (k->proto->listen(k, backlog) < 0)
    return -1;
  k->listening = 1;
  return 0;
}

/* Takes a waiting connection, with SOCK_NONBLOCK and SOCK_CLOEXEC flags. */
int accept4(int fd, struct sockaddr *a, socklen_t *len, int flags) {
  int nonblock;
  struct sock *k = sock_of(fd, &nonblock), *c;
  if (!k)
    return -1;
  if (!k->listening) {
    errno = EINVAL;
    return -1;
  }
  c = calloc(1, sizeof *c);
  if (!c) {
    errno = ENOBUFS;
    return -1;
  }
  c->family = k->family;
  c->type = k->type;
  if ((k->family == AF_INET ? inet_new(c) : __mac_unix_new(c)) < 0) {
    free(c);
    return -1;
  }
  if (k->proto->accept(k, c, nonblock) < 0) {
    int e = errno;
    c->proto->close(c);
    free(c);
    errno = e;
    return -1;
  }
  fd = new_fd(c, (flags & SOCK_NONBLOCK) != 0);
  if (fd >= 0 && a && len)
    c->proto->name(c, 1, a, len);
  return fd;
}

/* Takes a waiting connection. */
int accept(int fd, struct sockaddr *a, socklen_t *len) { return accept4(fd, a, len, 0); }

/* Connects a socket to an address. */
int connect(int fd, const struct sockaddr *a, socklen_t len) {
  int nonblock;
  struct sock *k = sock_of(fd, &nonblock);
  if (!k)
    return -1;
  if (!a) {
    errno = EINVAL;
    return -1;
  }
  return k->proto->connect(k, a, len, nonblock);
}

/* Sends bytes, to an address for a datagram socket. */
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *to, socklen_t len) {
  int nonblock;
  struct sock *k = sock_of(fd, &nonblock);
  if (!k)
    return -1;
  if (flags & MSG_OOB) {
    errno = EOPNOTSUPP;
    return -1;
  }
  return k->proto->send(k, buf, n, nonblock || (flags & MSG_DONTWAIT), to, len);
}

/* Sends bytes on a connected socket. */
ssize_t send(int fd, const void *buf, size_t n, int flags) { return sendto(fd, buf, n, flags, NULL, 0); }

/* Receives bytes, and the address of the sender. */
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *len) {
  int nonblock;
  struct sock *k = sock_of(fd, &nonblock);
  long got, total = 0;
  if (!k)
    return -1;
  if (flags & MSG_OOB) {
    errno = EINVAL;           /* MacTCP gives no urgent data */
    return -1;
  }
  nonblock = nonblock || (flags & MSG_DONTWAIT);
  do {
    got = k->proto->recv(k, (char *)buf + total, n - total, nonblock, (flags & MSG_PEEK) != 0, from, len);
    if (got <= 0)
      return total ? total : got;
    total += got;
  } while ((flags & MSG_WAITALL) && k->type == SOCK_STREAM && (size_t)total < n && !(flags & MSG_PEEK));
  return total;
}

/* Receives bytes on a connected socket. */
ssize_t recv(int fd, void *buf, size_t n, int flags) { return recvfrom(fd, buf, n, flags, NULL, NULL); }

/* Sends the buffers of a message as one block. */
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
  size_t total = 0, at = 0;
  char *joined;
  ssize_t r;
  int i;
  for (i = 0; i < msg->msg_iovlen; i++)
    total += msg->msg_iov[i].iov_len;
  joined = malloc(total ? total : 1);
  if (!joined) {
    errno = ENOBUFS;
    return -1;
  }
  for (i = 0; i < msg->msg_iovlen; i++) {
    memcpy(joined + at, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
    at += msg->msg_iov[i].iov_len;
  }
  r = sendto(fd, joined, total, flags, msg->msg_name, msg->msg_namelen);
  free(joined);
  return r;
}

/* Receives bytes into the buffers of a message. */
ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
  ssize_t r;
  if (msg->msg_iovlen < 1) {
    errno = EINVAL;
    return -1;
  }
  r = recvfrom(fd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags, msg->msg_name,
               msg->msg_name ? &msg->msg_namelen : NULL);
  msg->msg_controllen = 0;
  msg->msg_flags = 0;
  return r;
}

/* Ends one or both directions of a connection. */
int shutdown(int fd, int how) {
  struct sock *k = sock_of(fd, NULL);
  if (!k)
    return -1;
  if (how < SHUT_RD || how > SHUT_RDWR) {
    errno = EINVAL;
    return -1;
  }
  return k->proto->shutdown(k, how);
}

/* Gives an int option's value. */
static int put_int(void *value, socklen_t *len, int v) {
  if (!value || !len || *len < sizeof(int)) {
    errno = EINVAL;
    return -1;
  }
  *(int *)value = v;
  *len = sizeof(int);
  return 0;
}

/* Gives the value of a socket option. */
int getsockopt(int fd, int level, int name, void *value, socklen_t *len) {
  struct sock *k = sock_of(fd, NULL);
  if (!k)
    return -1;
  if (level == SOL_SOCKET) {
    switch (name) {
    case SO_ERROR: {
      int e = k->error;
      if (k->connecting && k->proto->ready(k) & POLLERR)
        e = k->error;
      k->error = 0;
      return put_int(value, len, e);
    }
    case SO_TYPE: return put_int(value, len, k->type);
    case SO_ACCEPTCONN: return put_int(value, len, k->listening);
    case SO_REUSEADDR: return put_int(value, len, k->reuse);
    case SO_KEEPALIVE: return put_int(value, len, k->keepalive);
    case SO_BROADCAST: return put_int(value, len, k->broadcast);
    case SO_RCVBUF: return put_int(value, len, TCP_BUFFER);
    case SO_SNDBUF: return put_int(value, len, SEGMENT);
    case SO_LINGER: {
      struct linger l = {0, 0};
      if (!value || !len || *len < sizeof l) {
        errno = EINVAL;
        return -1;
      }
      memcpy(value, &l, sizeof l);
      *len = sizeof l;
      return 0;
    }
    case SO_RCVTIMEO:
    case SO_SNDTIMEO: {
      struct timeval tv;
      long ms = name == SO_RCVTIMEO ? k->rcvtimeo : 0;
      if (!value || !len || *len < sizeof tv) {
        errno = EINVAL;
        return -1;
      }
      tv.tv_sec = ms / 1000;
      tv.tv_usec = (ms % 1000) * 1000;
      memcpy(value, &tv, sizeof tv);
      *len = sizeof tv;
      return 0;
    }
    }
  } else if (level == IPPROTO_TCP && name == TCP_NODELAY) {
    return put_int(value, len, 1);
  }
  errno = ENOPROTOOPT;
  return -1;
}

/* Sets a socket option. Some options that MacTCP does not have are
   accepted and ignored. */
int setsockopt(int fd, int level, int name, const void *value, socklen_t len) {
  struct sock *k = sock_of(fd, NULL);
  int v = value && len >= sizeof(int) ? *(const int *)value : 0;
  if (!k)
    return -1;
  if (level == SOL_SOCKET) {
    switch (name) {
    case SO_REUSEADDR: k->reuse = v; return 0;
    case SO_KEEPALIVE: k->keepalive = v; return 0;
    case SO_BROADCAST: k->broadcast = v; return 0;
    case SO_RCVBUF:
    case SO_SNDBUF:
    case SO_LINGER:
    case SO_OOBINLINE:
    case SO_SNDTIMEO:
      return 0;               /* MacTCP has fixed sizes; accepted */
    case SO_RCVTIMEO: {
      struct timeval tv;
      if (!value || len < sizeof tv) {
        errno = EINVAL;
        return -1;
      }
      memcpy(&tv, value, sizeof tv);
      k->rcvtimeo = tv.tv_sec * 1000 + tv.tv_usec / 1000;
      return 0;
    }
    }
  } else if (level == IPPROTO_TCP && name == TCP_NODELAY) {
    return 0;
  } else if (level == IPPROTO_IP && (name == IP_TOS || name == IP_TTL)) {
    return 0;
  }
  errno = ENOPROTOOPT;
  return -1;
}

/* Gives the local or the peer address of a socket, for unix.c. */
int __mac_sock_name(struct sock *k, int peer, struct sockaddr *a, socklen_t *len) {
  return k->proto->name(k, peer, a, len);
}

/* Gives the local address of a socket. */
int getsockname(int fd, struct sockaddr *a, socklen_t *len) {
  struct sock *k = sock_of(fd, NULL);
  if (!k)
    return -1;
  if (!a || !len) {
    errno = EINVAL;
    return -1;
  }
  return k->proto->name(k, 0, a, len);
}

/* Gives the address of the peer of a connected socket. */
int getpeername(int fd, struct sockaddr *a, socklen_t *len) {
  struct sock *k = sock_of(fd, NULL);
  if (!k)
    return -1;
  if (!a || !len) {
    errno = EINVAL;
    return -1;
  }
  return k->proto->name(k, 1, a, len);
}
