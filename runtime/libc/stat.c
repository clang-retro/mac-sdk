/* stat, fstat, lstat, ftruncate, mkdir, rmdir and utime over the File
 * Manager's catalog calls. A folder is S_IFDIR, a file S_IFREG with the
 * size of its data fork; a locked file has no write permission. The
 * dates are the Mac's, which count local time, given in UTC.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include "macos.h"

/* Seconds from 1904-01-01 to 1970-01-01. */
#define MAC_EPOCH 2082844800UL

/* The catalog flag of a folder, and of a locked file. */
#define ATTR_DIR 0x10
#define ATTR_LOCKED 0x01

/* Converts a Mac date (local seconds since 1904) to a UTC time_t. */
long long __mac_date_to_time(unsigned long date) {
  return (long long)date - (long long)MAC_EPOCH - __mac_gmt_delta();
}

/* Converts a UTC time_t to a Mac date (local seconds since 1904). */
unsigned long __mac_time_to_date(long long t) {
  return (unsigned long)(t + (long long)MAC_EPOCH + __mac_gmt_delta());
}

/* Gets the catalog information of the item at a POSIX path into pb.
   name holds the Mac name that pb points to. Returns the File Manager
   result. */
OSErr __mac_cat_info(const char *path, CInfoPBRec *pb, Str255 name) {
  memset(pb, 0, sizeof *pb);
  if (path[0] == 0 || (path[0] == '.' && path[1] == 0)) {
    /* The current folder itself. */
    name[0] = 0;
    pb->dirInfo.ioNamePtr = name;
    pb->dirInfo.ioFDirIndex = -1;
  } else {
    if (__mac_path(path, name) < 0)
      return bdNamErr;
    pb->hFileInfo.ioNamePtr = name;
  }
  return PBGetCatInfoSync(pb);
}

/* Fills st from the catalog information in pb. */
static void fill_stat(const CInfoPBRec *pb, struct stat *st) {
  unsigned long cr, md;
  memset(st, 0, sizeof *st);
  st->st_dev = (dev_t)(unsigned short)pb->hFileInfo.ioVRefNum;
  st->st_nlink = 1;
  st->st_blksize = 512;
  if (pb->hFileInfo.ioFlAttrib & ATTR_DIR) {
    st->st_mode = S_IFDIR | 0755;
    st->st_ino = (ino_t)pb->dirInfo.ioDrDirID;
    cr = pb->dirInfo.ioDrCrDat;
    md = pb->dirInfo.ioDrMdDat;
  } else {
    st->st_mode = S_IFREG |
                  ((pb->hFileInfo.ioFlAttrib & ATTR_LOCKED) ? 0444 : 0644);
    st->st_ino = (ino_t)pb->hFileInfo.ioDirID;
    st->st_size = pb->hFileInfo.ioFlLgLen;
    st->st_blocks = (pb->hFileInfo.ioFlPyLen + 511) / 512;
    cr = pb->hFileInfo.ioFlCrDat;
    md = pb->hFileInfo.ioFlMdDat;
  }
  st->st_mtim.tv_sec = (time_t)__mac_date_to_time(md);
  st->st_atim.tv_sec = st->st_mtim.tv_sec;
  st->st_ctim.tv_sec = (time_t)__mac_date_to_time(cr);
}

/* Sets errno from a File Manager error and returns -1. */
static int fail(OSErr err) {
  errno = __mac_errno(err);
  return -1;
}

/* Gets the information of the file or folder at path. */
int stat(const char *restrict path, struct stat *restrict st) {
  CInfoPBRec pb;
  Str255 name;
  OSErr err = __mac_cat_info(path, &pb, name);
  if (err != noErr)
    return fail(err);
  fill_stat(&pb, st);
  return 0;
}

/* The Mac has no symbolic links; an alias file is a file. */
int lstat(const char *restrict path, struct stat *restrict st) {
  return stat(path, st);
}

/* Gets the information of an open file. The console descriptors 0 to 2
   are character devices. */
int fstat(int fd, struct stat *st) {
  FCBPBRec fcb;
  CInfoPBRec pb;
  Str255 name;
  OSErr err;
  short ref;
  if (fd >= 0 && fd < 3) {
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFCHR | 0666;
    return 0;
  }
  ref = __mac_fd_ref(fd);
  if (!ref) {
    errno = EBADF;
    return -1;
  }
  memset(&fcb, 0, sizeof fcb);
  fcb.ioNamePtr = name;
  fcb.ioRefNum = ref;
  err = PBGetFCBInfoSync(&fcb);
  if (err != noErr)
    return fail(err);
  memset(&pb, 0, sizeof pb);
  pb.hFileInfo.ioNamePtr = name;
  pb.hFileInfo.ioVRefNum = fcb.ioFCBVRefNum;
  pb.hFileInfo.ioDirID = fcb.ioFCBParID;
  err = PBGetCatInfoSync(&pb);
  if (err != noErr)
    return fail(err);
  fill_stat(&pb, st);
  /* The size of the open file, with what is not yet written out. */
  st->st_size = fcb.ioFCBEOF;
  return 0;
}

/* Sets the length of the data fork of an open file. */
int ftruncate(int fd, off_t length) {
  short ref = __mac_fd_ref(fd);
  OSErr err;
  if (!ref) {
    errno = EBADF;
    return -1;
  }
  if (length < 0 || length > 0x7FFFFFFF) {
    errno = EINVAL;
    return -1;
  }
  err = SetEOF(ref, (long)length);
  return err == noErr ? 0 : fail(err);
}

/* Makes a folder. The Mac has no permissions, so mode is not used. */
int mkdir(const char *path, mode_t mode) {
  HParamBlockRec pb;
  Str255 name;
  OSErr err;
  (void)mode;
  if (__mac_path(path, name) < 0)
    return -1;
  memset(&pb, 0, sizeof pb);
  pb.fileParam.ioNamePtr = name;
  err = PBDirCreateSync(&pb);
  if (err == dupFNErr) {
    errno = EEXIST;
    return -1;
  }
  return err == noErr ? 0 : fail(err);
}

/* Deletes an empty folder. */
int rmdir(const char *path) {
  CInfoPBRec pb;
  Str255 name;
  OSErr err = __mac_cat_info(path, &pb, name);
  if (err != noErr)
    return fail(err);
  if (!(pb.hFileInfo.ioFlAttrib & ATTR_DIR)) {
    errno = ENOTDIR;
    return -1;
  }
  if (__mac_path(path, name) < 0)
    return -1;
  err = FSDelete(name, 0);
  if (err == fBsyErr) {
    errno = ENOTEMPTY;
    return -1;
  }
  return err == noErr ? 0 : fail(err);
}

/* Sets the modification date of a file or folder: to times->modtime,
   or to the current time when times is NULL. */
int utime(const char *path, const struct utimbuf *times) {
  CInfoPBRec pb;
  Str255 name;
  unsigned long date;
  OSErr err = __mac_cat_info(path, &pb, name);
  if (err != noErr)
    return fail(err);
  if (times) {
    date = __mac_time_to_date(times->modtime);
  } else {
    GetDateTime(&date);
  }
  if (pb.hFileInfo.ioFlAttrib & ATTR_DIR)
    pb.dirInfo.ioDrMdDat = date;
  else
    pb.hFileInfo.ioFlMdDat = date;
  /* GetCatInfo put the item's own ID in ioDirID. With a name, SetCatInfo
     must find the item as GetCatInfo did: the name, relative to the
     current folder (ID 0). Without a name (the current folder), the
     folder's own ID names it. */
  if (name[0] != 0)
    pb.hFileInfo.ioDirID = 0;
  pb.hFileInfo.ioFDirIndex = 0;
  err = PBSetCatInfoSync(&pb);
  return err == noErr ? 0 : fail(err);
}
