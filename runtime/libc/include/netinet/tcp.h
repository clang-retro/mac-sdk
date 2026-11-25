/* TCP options for the classic Mac OS (runtime/libc/socket.c). MacTCP
 * sends at once, so TCP_NODELAY is accepted and changes nothing.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef _NETINET_TCP_H
#define _NETINET_TCP_H

#define TCP_NODELAY 1

#endif
