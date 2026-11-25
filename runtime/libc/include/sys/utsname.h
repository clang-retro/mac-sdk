/* uname for the classic Mac OS (runtime/libc/posix.c).
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

struct utsname {
  char sysname[32];  /* "MacOS" */
  char nodename[32]; /* "Macintosh" */
  char release[32];  /* the System's version, such as "7.5.3" */
  char version[32];  /* the same */
  char machine[32];  /* "m68k" or "powerpc" */
};

#ifdef __cplusplus
extern "C" {
#endif

int uname(struct utsname *u);

#ifdef __cplusplus
}
#endif

#endif
