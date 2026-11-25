/* mmap and the other memory mapping functions, and POSIX shared memory,
 * for the classic Mac OS. The Mac has no memory mapping calls for a
 * program, so a mapping is a block of the heap:
 *
 * - An anonymous mapping is a zeroed block.
 * - A file mapping is a block with a copy of the file's bytes. msync and
 *   munmap write the block of a MAP_SHARED mapping with PROT_WRITE back
 *   to the file. Two MAP_SHARED mappings of the same bytes of a file
 *   share one block, as they share memory on other systems.
 * - mprotect changes nothing: the memory is always readable and
 *   writable.
 * - A shared memory object (shm_open) is a file in the Temporary Items
 *   folder, or in the current folder before System 7.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <Folders.h>
#include <Gestalt.h>
#include "macos.h"

int shm_open(const char *name, int flags, mode_t mode);
int shm_unlink(const char *name);

/* The alignment of a mapping's address, and of its length. */
#define ALIGN 16

/* One mapping. */
struct mapping {
  struct mapping *next;
  char *addr;
  size_t len;
  int refs;        /* the mmap calls that gave this block */
  int fd;          /* a copy (dup) of the file's descriptor, or -1 */
  short vol;       /* the file's volume and file number, for sharing */
  long file;
  off_t offset;    /* where the block starts in the file */
  int write_back;  /* MAP_SHARED with PROT_WRITE */
};

static struct mapping *mappings;

/* Gives the mapping that holds the byte at p, or NULL. */
static struct mapping *find(const void *p) {
  struct mapping *m;
  for (m = mappings; m; m = m->next)
    if ((const char *)p >= m->addr && (const char *)p < m->addr + m->len)
      return m;
  return NULL;
}

/* Gets the volume and file number of the open file fd. */
static int file_id(int fd, short *vol, long *file) {
  FCBPBRec fcb;
  OSErr err;
  memset(&fcb, 0, sizeof fcb);
  fcb.ioRefNum = __mac_fd_ref(fd);
  if (!fcb.ioRefNum) {
    errno = EBADF;
    return -1;
  }
  err = PBGetFCBInfoSync(&fcb);
  if (err != noErr) {
    errno = __mac_errno(err);
    return -1;
  }
  *vol = fcb.ioFCBVRefNum;
  *file = fcb.ioFCBFlNm;
  return 0;
}

/* Writes the bytes [from, from + n) of mapping m back to its file. The
   file does not become longer: bytes after its end stay in memory. */
static int write_back(struct mapping *m, size_t from, size_t n) {
  struct stat st;
  off_t at = m->offset + (off_t)from;
  if (!m->write_back)
    return 0;
  if (fstat(m->fd, &st) != 0)
    return -1;
  if (at >= st.st_size)
    return 0;
  if ((off_t)n > st.st_size - at)
    n = (size_t)(st.st_size - at);
  return pwrite(m->fd, m->addr + from, n, at) == (ssize_t)n ? 0 : -1;
}

/* Maps len bytes: zeroed memory for MAP_ANONYMOUS, otherwise the bytes
   of file fd from offset. With MAP_FIXED, addr must be in an existing
   mapping: those bytes become zero, as new anonymous pages are. Gives
   MAP_FAILED with errno set on an error. */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
  struct mapping *m;
  short vol = 0;
  long file = 0;
  if (len == 0 || offset < 0) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  if (flags & MAP_FIXED) {
    m = find(addr);
    if (!m || !(flags & MAP_ANONYMOUS) ||
        (char *)addr + len > m->addr + m->len) {
      errno = ENOMEM;
      return MAP_FAILED;
    }
    memset(addr, 0, len);
    return addr;
  }
  if (!(flags & MAP_ANONYMOUS)) {
    if (file_id(fd, &vol, &file) != 0)
      return MAP_FAILED;
    /* Another MAP_SHARED mapping of the same bytes gives its block. */
    if (flags & MAP_SHARED)
      for (m = mappings; m; m = m->next)
        if (m->fd >= 0 && m->vol == vol && m->file == file &&
            offset >= m->offset &&
            offset + (off_t)len <= m->offset + (off_t)m->len) {
          m->refs++;
          if (prot & PROT_WRITE)
            m->write_back = 1;
          return m->addr + (offset - m->offset);
        }
  }
  m = malloc(sizeof *m);
  if (!m) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  memset(m, 0, sizeof *m);
  m->len = (len + ALIGN - 1) & ~(size_t)(ALIGN - 1);
  m->addr = aligned_alloc(ALIGN, m->len);
  if (!m->addr) {
    free(m);
    errno = ENOMEM;
    return MAP_FAILED;
  }
  memset(m->addr, 0, m->len);
  m->refs = 1;
  m->fd = -1;
  if (!(flags & MAP_ANONYMOUS)) {
    ssize_t got = pread(fd, m->addr, len, offset);
    if (got < 0 || (m->fd = dup(fd)) < 0) {
      free(m->addr);
      free(m);
      return MAP_FAILED;
    }
    m->vol = vol;
    m->file = file;
    m->offset = offset;
    m->write_back = (flags & MAP_SHARED) && (prot & PROT_WRITE);
  }
  m->next = mappings;
  mappings = m;
  return m->addr;
}

/* Ends a mapping that mmap gave at addr. The block is released when its
   last mmap ends. A part of a mapping cannot be unmapped: munmap of a
   part does nothing. */
int munmap(void *addr, size_t len) {
  struct mapping **l, *m;
  (void)len;
  for (l = &mappings; *l; l = &(*l)->next)
    if ((char *)addr >= (*l)->addr && (char *)addr < (*l)->addr + (*l)->len)
      break;
  m = *l;
  if (!m) {
    errno = EINVAL;
    return -1;
  }
  if (--m->refs > 0)
    return 0;
  write_back(m, 0, m->len);
  *l = m->next;
  if (m->fd >= 0)
    close(m->fd);
  free(m->addr);
  free(m);
  return 0;
}

/* Writes the changes of a MAP_SHARED file mapping back to the file. */
int msync(void *addr, size_t len, int flags) {
  struct mapping *m = find(addr);
  size_t from;
  (void)flags;
  if (!m) {
    errno = ENOMEM;
    return -1;
  }
  from = (size_t)((char *)addr - m->addr);
  if (len > m->len - from)
    len = m->len - from;
  return write_back(m, from, len);
}

/* The memory is always readable and writable: nothing changes. */
int mprotect(void *addr, size_t len, int prot) {
  (void)len;
  (void)prot;
  if (!find(addr)) {
    errno = ENOMEM;
    return -1;
  }
  return 0;
}

/* Advice about the use of a mapping: none changes anything. */
int posix_madvise(void *addr, size_t len, int advice) {
  (void)addr;
  (void)len;
  (void)advice;
  return 0;
}

/* Puts the POSIX path of the file for shared memory object name in path
   (of size bytes): in the Temporary Items folder of the System disk, or
   in the current folder when there is no Folder Manager. A "/" in the
   name becomes "_". */
static int shm_path(const char *name, char *path, size_t size) {
  long attr = 0;
  short vol = 0;
  long dir = 0;
  size_t n;
  if (!name || !*name) {
    errno = EINVAL;
    return -1;
  }
  while (*name == '/')
    name++;
  if (Gestalt(gestaltFindFolderAttr, &attr) != noErr ||
      !(attr & 1L /* the Folder Manager is present */) ||
      FindFolder(kOnSystemDisk, kTemporaryFolderType, true, &vol, &dir) != noErr) {
    vol = 0;
    dir = 0;
  }
  if (!__mac_folder_path(vol, dir, path, size))
    return -1;
  n = strlen(path);
  if (n + 5 + strlen(name) + 1 > size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(path + n, "/shm.", 5);
  n += 5;
  for (; *name; name++)
    path[n++] = *name == '/' ? '_' : *name;
  path[n] = 0;
  return 0;
}

/* Opens or makes the shared memory object name. Gives a file descriptor
   for mmap and ftruncate. */
int shm_open(const char *name, int flags, mode_t mode) {
  char path[1024];
  if (shm_path(name, path, sizeof path) != 0)
    return -1;
  return open(path, flags, mode);
}

/* Deletes the shared memory object name. */
int shm_unlink(const char *name) {
  char path[1024];
  if (shm_path(name, path, sizeof path) != 0)
    return -1;
  return unlink(path);
}
