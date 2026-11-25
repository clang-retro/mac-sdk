/* Internet addresses for the classic Mac OS (runtime/libc/socket.c).
 * There is no IPv6 network: the IPv6 types are here so that programs
 * compile, and AF_INET6 sockets fail with EAFNOSUPPORT.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

struct in_addr {
  in_addr_t s_addr;
};

struct sockaddr_in {
  sa_family_t sin_family;
  in_port_t sin_port;
  struct in_addr sin_addr;
  unsigned char sin_zero[8];
};

struct in6_addr {
  union {
    unsigned char __s6_addr[16];
    __uint16_t __s6_addr16[8];
    __uint32_t __s6_addr32[4];
  } __in6_u;
};
#define s6_addr __in6_u.__s6_addr

struct sockaddr_in6 {
  sa_family_t sin6_family;
  in_port_t sin6_port;
  __uint32_t sin6_flowinfo;
  struct in6_addr sin6_addr;
  __uint32_t sin6_scope_id;
};

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41

#define INADDR_ANY ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)

#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

#define IP_TOS 1
#define IP_TTL 2
#define IPV6_V6ONLY 26

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;
#define IN6ADDR_ANY_INIT {{{0}}}
#define IN6ADDR_LOOPBACK_INIT {{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}}}

#endif
