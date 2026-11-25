/* Byte order and address text for the classic Mac OS (runtime/libc/netdb.c).
 * The 68k and the PowerPC are big-endian: the network byte order.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#define __ARPA_INET_H__

#include <sys/types.h>

#define htonl(x) ((__uint32_t)(x))
#define htons(x) ((__uint16_t)(x))
#define ntohl(x) ((__uint32_t)(x))
#define ntohs(x) ((__uint16_t)(x))

struct in_addr;
#ifndef _SOCKLEN_T_DECLARED
#define _SOCKLEN_T_DECLARED
typedef __uint32_t socklen_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

in_addr_t inet_addr(const char *text);
int inet_aton(const char *text, struct in_addr *addr);
char *inet_ntoa(struct in_addr addr);
const char *inet_ntop(int family, const void *addr, char *buf, socklen_t size);
int inet_pton(int family, const char *text, void *addr);

#ifdef __cplusplus
}
#endif

#endif
