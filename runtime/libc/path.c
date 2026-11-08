/* POSIX paths to Mac paths.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include "macos.h"

int __mac_path(const char *path, Str255 out) {
  const char *p = path;
  unsigned n = 0;
  int slash = 0;

  for (; *p; p++)
    if (*p == '/')
      slash = 1;
  if (!slash) {
    /* A Mac path (or a plain name in the current folder). */
    for (p = path; *p; p++) {
      if (n == 255)
        goto toolong;
      out[++n] = (unsigned char)*p;
    }
    out[0] = (unsigned char)n;
    return 0;
  }
  p = path;
  if (*p == '/') {
    /* Absolute: the first component is the volume name. */
    while (*p == '/')
      p++;
  } else {
    out[++n] = ':';             /* relative to the current folder */
  }
  while (*p) {
    const char *c = p;
    unsigned len = 0;
    while (c[len] && c[len] != '/')
      len++;
    if (len == 1 && c[0] == '.') {
      /* "." adds nothing */
    } else if (len == 2 && c[0] == '.' && c[1] == '.') {
      if (n == 255)
        goto toolong;
      out[++n] = ':';           /* "::" is the parent folder */
    } else if (len) {
      unsigned i;
      if (n + len + 1 > 255)
        goto toolong;
      for (i = 0; i < len; i++)
        out[++n] = (unsigned char)(c[i] == ':' ? '/' : c[i]);
      out[++n] = ':';
    }
    p = c + len;
    while (*p == '/')
      p++;
  }
  /* Drop the last ":" after a file name, but keep it after a lone volume
     name ("/Disk" is "Disk:"). */
  if (n > 1 && out[n] == ':' && out[n - 1] != ':') {
    unsigned i, colons = 0;
    for (i = 1; i < n; i++)
      if (out[i] == ':')
        colons++;
    if (colons || path[0] != '/')
      n--;
  }
  out[0] = (unsigned char)n;
  return 0;
toolong:
  errno = ENAMETOOLONG;
  return -1;
}
