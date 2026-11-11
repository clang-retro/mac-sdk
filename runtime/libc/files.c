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

/* The directory ID of the folder that path[1..len] names (a Mac path
   ending in ":"), or of the current folder when len is 0. */
static OSErr folder_id(const unsigned char *path, unsigned len, long *dir) {
  CInfoPBRec pb;
  Str255 f;
  unsigned i;
  OSErr err;
  pb.dirInfo.ioCompletion = 0;
  pb.dirInfo.ioVRefNum = 0;
  pb.dirInfo.ioDrDirID = 0;
  if (len) {
    for (i = 1; i <= len; i++)
      f[i] = path[i];
    f[0] = (unsigned char)len;
    pb.dirInfo.ioNamePtr = f;
    pb.dirInfo.ioFDirIndex = 0;
  } else {
    pb.dirInfo.ioNamePtr = NULL;
    pb.dirInfo.ioFDirIndex = -1;
  }
  err = PBGetCatInfoSync(&pb);
  if (err == noErr && !(pb.dirInfo.ioFlAttrib & 0x10))
    err = dirNFErr;
  *dir = pb.dirInfo.ioDrDirID;
  return err;
}

/* Moves path into the folder dir (same volume only). */
static OSErr move(unsigned char *path, long dir) {
  CMovePBRec pb;
  pb.ioCompletion = 0;
  pb.ioNamePtr = path;
  pb.ioVRefNum = 0;
  pb.ioNewName = NULL;
  pb.ioNewDirID = dir;
  pb.ioDirID = 0;
  return PBCatMoveSync(&pb);
}

static int fail(OSErr err) {
  errno = err == badMovErr ? EXDEV : __mac_errno(err);
  return -1;
}

/* Within a folder this is the File Manager's Rename. Into another folder
   it is Rename and CatMove (HFS volumes; EXDEV across volumes). As in
   POSIX, a file already at the new path is replaced. */
int rename(const char *from, const char *to) {
  Str255 a, b, mid;
  OSErr err;
  long dir;
  unsigned i, sa = 0, sb = 0, same;
  if (__mac_path(from, a) < 0 || __mac_path(to, b) < 0)
    return -1;
  for (i = 1; i <= a[0]; i++)
    if (a[i] == ':')
      sa = i;
  for (i = 1; i <= b[0]; i++)
    if (b[i] == ':')
      sb = i;
  same = sa == sb;
  for (i = 1; same && i <= sa; i++)
    same = a[i] == b[i];
  if (same) {
    err = Rename(a, 0, b);
    if (err == dupFNErr) {
      err = FSDelete(b, 0);
      if (err == noErr)
        err = Rename(a, 0, b);
    }
    return err == noErr ? 0 : fail(err);
  }
  err = folder_id(b, sb, &dir);
  if (err != noErr)
    return fail(err);
  err = FSDelete(b, 0);
  if (err != noErr && err != fnfErr)
    return fail(err);
  /* Give the file its new name in its old folder, then move it. */
  if (sa + b[0] - sb > 255) {
    errno = ENAMETOOLONG;
    return -1;
  }
  for (i = 1; i <= sa; i++)
    mid[i] = a[i];
  for (i = sb + 1; i <= b[0]; i++)
    mid[sa + i - sb] = b[i];
  mid[0] = (unsigned char)(sa + b[0] - sb);
  same = a[0] - sa == b[0] - sb;
  for (i = 1; same && i <= a[0] - sa; i++)
    same = a[sa + i] == b[sb + i];
  if (!same) {
    err = Rename(a, 0, mid);
    if (err != noErr)
      return fail(err);
  }
  err = move(mid, dir);
  if (err != noErr) {
    if (!same)
      Rename(mid, 0, a);
    return fail(err);
  }
  return 0;
}
