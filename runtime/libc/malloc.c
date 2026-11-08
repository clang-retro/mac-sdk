/* malloc and friends over the Memory Manager: each block is a
 * nonrelocatable block (NewPtr) in the application heap, so memory comes
 * from the program's partition (its SIZE resource) and goes back to it
 * when freed.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <Memory.h>

void *malloc(size_t n) {
  Ptr p = NewPtr((Size)n);
  if (!p)
    errno = ENOMEM;
  return p;
}

void free(void *p) {
  if (p)
    DisposePtr((Ptr)p);
}

void *calloc(size_t count, size_t size) {
  Ptr p;
  if (size && count > (size_t)0x7FFFFFFF / size) {
    errno = ENOMEM;
    return NULL;
  }
  p = NewPtrClear((Size)(count * size));
  if (!p)
    errno = ENOMEM;
  return p;
}

void *realloc(void *old, size_t n) {
  Ptr p;
  Size have;
  if (!old)
    return malloc(n);
  /* Grow or shrink in place when the heap allows it. */
  SetPtrSize((Ptr)old, (Size)n);
  if (MemError() == noErr)
    return old;
  p = NewPtr((Size)n);
  if (!p) {
    errno = ENOMEM;
    return NULL;
  }
  have = GetPtrSize((Ptr)old);
  memcpy(p, old, have < (Size)n ? (size_t)have : n);
  DisposePtr((Ptr)old);
  return p;
}
