/* What socket.c (the socket calls and AF_INET) and unix.c (AF_UNIX)
 * share.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef MACOS_SOCKET_H
#define MACOS_SOCKET_H

#include <sys/socket.h>
#include <netinet/in.h>

struct sock;

/* The functions of one address family. They give -1 with errno set on
   an error. nonblock tells them to give EAGAIN instead of waiting. */
struct proto {
  int (*bind)(struct sock *k, const struct sockaddr *a, socklen_t len);
  int (*listen)(struct sock *k, int backlog);
  /* Puts the next connection into c, a new socket of the family. */
  int (*accept)(struct sock *k, struct sock *c, int nonblock);
  int (*connect)(struct sock *k, const struct sockaddr *a, socklen_t len, int nonblock);
  long (*send)(struct sock *k, const void *buf, size_t n, int nonblock, const struct sockaddr *to,
               socklen_t tolen);
  long (*recv)(struct sock *k, void *buf, size_t n, int nonblock, int peek, struct sockaddr *from,
               socklen_t *fromlen);
  short (*ready)(struct sock *k);   /* the poll events true now */
  long (*nread)(struct sock *k);    /* the bytes a read gives now */
  int (*shutdown)(struct sock *k, int how);
  void (*close)(struct sock *k);    /* frees k->impl */
  /* The socket's own address (peer 0) or the other end's (peer 1). */
  int (*name)(struct sock *k, int peer, struct sockaddr *a, socklen_t *len);
};

struct sock {
  int family, type;
  const struct proto *proto;
  void *impl;
  int listening, connecting, connected;
  int rdshut, wrshut;
  int error;                     /* for SO_ERROR */
  int reuse, keepalive, broadcast;
  long rcvtimeo;                 /* SO_RCVTIMEO in milliseconds, 0: none */
  in_addr_t host, peer_host;     /* AF_INET addresses, network order */
  in_port_t port, peer_port;
};

/* Makes the AF_UNIX part of a new socket (unix.c). */
int __mac_unix_new(struct sock *k);

/* Ends a connect in progress whose call has ended (socket.c). */
int __mac_sock_connected(struct sock *k);

/* The proto->name of k (socket.c). */
int __mac_sock_name(struct sock *k, int peer, struct sockaddr *a, socklen_t *len);

/* Our IP address in network order, or 0 when MacTCP is not there
   (socket.c). */
in_addr_t __mac_ip_address(void);

#endif
