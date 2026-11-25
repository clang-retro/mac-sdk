/* AF_UNIX socket addresses for the classic Mac OS (runtime/libc/socket.c).
 * The path names a port of the PPC Toolbox on this Mac: it is not a
 * file.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef _SYS_UN_H
#define _SYS_UN_H

#include <sys/socket.h>

struct sockaddr_un {
  sa_family_t sun_family;
  char sun_path[108];
};

#endif
