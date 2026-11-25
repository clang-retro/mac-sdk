/* opendir, readdir, rewinddir and closedir over the File Manager's
 * indexed GetCatInfo, and getcwd. A "/" in a Mac name is ":" in the POSIX
 * name, as runtime/libc/path.c reads it back.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "macos.h"

#define ATTR_DIR 0x10

/* The DIR fields: fd holds the volume reference number, count the folder's
   directory ID and offset the index of the next item (from 1). */

DIR *opendir(const char *path) {
  CInfoPBRec pb;
  Str255 name;
  DIR *d;
  OSErr err = __mac_cat_info(path, &pb, name);
  if (err != noErr) {
    errno = __mac_errno(err);
    return NULL;
  }
  if (!(pb.dirInfo.ioFlAttrib & ATTR_DIR)) {
    errno = ENOTDIR;
    return NULL;
  }
  d = malloc(sizeof *d);
  if (!d) {
    errno = ENOMEM;
    return NULL;
  }
  memset(d, 0, sizeof *d);
  d->fd = pb.dirInfo.ioVRefNum;
  d->count = (size_t)pb.dirInfo.ioDrDirID;
  d->offset = 1;
  return d;
}

/* Gives the next item of a folder, or NULL at the end. */
struct dirent *readdir(DIR *d) {
  CInfoPBRec pb;
  Str255 name;
  unsigned i;
  OSErr err;
  memset(&pb, 0, sizeof pb);
  pb.dirInfo.ioNamePtr = name;
  pb.dirInfo.ioVRefNum = (short)d->fd;
  pb.dirInfo.ioFDirIndex = (short)d->offset;
  pb.dirInfo.ioDrDirID = (long)d->count;
  err = PBGetCatInfoSync(&pb);
  if (err == fnfErr)
    return NULL;                /* the end: errno is not changed */
  if (err != noErr) {
    errno = __mac_errno(err);
    return NULL;
  }
  d->offset++;
  for (i = 0; i < name[0]; i++)
    d->dirent.d_name[i] = name[i + 1] == '/' ? ':' : (char)name[i + 1];
  d->dirent.d_name[name[0]] = 0;
  if (pb.dirInfo.ioFlAttrib & ATTR_DIR) {
    d->dirent.d_type = DT_DIR;
    d->dirent.d_ino = (ino_t)pb.dirInfo.ioDrDirID;
  } else {
    d->dirent.d_type = DT_REG;
    d->dirent.d_ino = (ino_t)pb.hFileInfo.ioDirID;
  }
  return &d->dirent;
}

/* Starts a folder listing again from its first item. */
void rewinddir(DIR *d) {
  d->offset = 1;
}

/* Ends a folder listing and releases its memory. */
int closedir(DIR *d) {
  free(d);
  return 0;
}

/* The POSIX path of the current folder: "/Volume/Folder/...", from the
   folder up to the volume's root (directory ID 2). */
char *getcwd(char *buf, size_t size) {
  char path[1024];
  size_t len = 0, at = sizeof path;
  CInfoPBRec pb;
  Str255 name;
  long dir = 0;
  short vol = 0;
  OSErr err;
  unsigned i;
  path[--at] = 0;
  for (;;) {
    memset(&pb, 0, sizeof pb);
    pb.dirInfo.ioNamePtr = name;
    pb.dirInfo.ioVRefNum = vol;
    pb.dirInfo.ioFDirIndex = -1;
    pb.dirInfo.ioDrDirID = dir;
    err = PBGetCatInfoSync(&pb);
    if (err != noErr) {
      errno = __mac_errno(err);
      return NULL;
    }
    if (at < (size_t)name[0] + 1) {
      errno = ERANGE;
      return NULL;
    }
    for (i = name[0]; i > 0; i--)
      path[--at] = name[i] == '/' ? ':' : (char)name[i];
    path[--at] = '/';
    vol = pb.dirInfo.ioVRefNum;
    if (pb.dirInfo.ioDrDirID == 2 || pb.dirInfo.ioDrParID <= 1)
      break;
    dir = pb.dirInfo.ioDrParID;
  }
  len = sizeof path - 1 - at;
  if (!buf) {
    size = size > len ? size : len + 1;
    buf = malloc(size);
    if (!buf) {
      errno = ENOMEM;
      return NULL;
    }
  }
  if (size < len + 1) {
    errno = ERANGE;
    return NULL;
  }
  memcpy(buf, path + at, len + 1);
  return buf;
}
