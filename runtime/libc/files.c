/* unlink and rename over the File Manager.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include "macos.h"

int unlink(const char *path) {
  Str255 name;
  OSErr err;
  if (__mac_path(path, name) < 0)
    return -1;
  err = FSDelete(name, 0);
  if (err != noErr) {
    errno = __mac_errno(err);
    return -1;
  }
  return 0;
}

/* The File Manager renames within a folder only, so the new name's
   folder must be the old one's; rename fails with EXDEV otherwise. */
int rename(const char *from, const char *to) {
  Str255 a, b;
  OSErr err;
  unsigned i, sa = 0, sb = 0;
  if (__mac_path(from, a) < 0 || __mac_path(to, b) < 0)
    return -1;
  for (i = 1; i <= a[0]; i++)
    if (a[i] == ':')
      sa = i;
  for (i = 1; i <= b[0]; i++)
    if (b[i] == ':')
      sb = i;
  if (sa != sb) {
    errno = EXDEV;
    return -1;
  }
  for (i = 1; i <= sa; i++)
    if (a[i] != b[i]) {
      errno = EXDEV;
      return -1;
    }
  err = Rename(a, 0, b);
  if (err == dupFNErr) {
    /* POSIX rename replaces an existing file. */
    err = FSDelete(b, 0);
    if (err == noErr)
      err = Rename(a, 0, b);
  }
  if (err != noErr) {
    errno = __mac_errno(err);
    return -1;
  }
  return 0;
}
