/* malloc and friends over the Memory Manager: each block is a
 * nonrelocatable block (NewPtr) in the application heap, so memory comes
 * from the program's partition (its SIZE resource) and goes back to it
 * when freed. aligned_alloc, posix_memalign and memalign take a larger
 * block when NewPtr's is not aligned enough, and remember it in a list
 * that free and realloc look in first.
 *
 * When the heap has no room for a block of TEMP_MIN bytes or more,
 * malloc takes it from temporary memory (TempNewHandle, locked), which
 * is the free memory outside all partitions. Thus a program with a small
 * partition can still get a large buffer, such as a picture of the whole
 * screen. The Process Manager frees temporary memory when the program
 * quits. Such blocks are also in a list.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <Gestalt.h>
#include <Memory.h>

/* The smallest block that can come from temporary memory. */
#define TEMP_MIN 65536L
/* The bits of gestaltOSAttr: the temporary memory calls, real temporary
   memory, and temporary memory that is freed when the program quits. */
#define TEMP_BITS ((1L << 4) | (1L << 5) | (1L << 6))

/* Just before each such aligned pointer: the next one, and the NewPtr
   block it is in. */
struct aligned {
  struct aligned *next;
  Ptr block;
};
static struct aligned *aligned_blocks;

/* The entry for p if it is one of those, taken out of the list. */
static struct aligned *take_aligned(void *p) {
  struct aligned **l;
  for (l = &aligned_blocks; *l; l = &(*l)->next)
    if ((void *)(*l + 1) == p) {
      struct aligned *a = *l;
      *l = a->next;
      return a;
    }
  return NULL;
}

/* Just before each block from temporary memory: the next one, its
   handle, and its size. */
struct temp {
  struct temp *next;
  Handle handle;
  size_t size;
  long pad; /* makes the header 16 bytes */
};
static struct temp *temp_blocks;

/* The entry for p if it is a block from temporary memory, or NULL. */
static struct temp *find_temp(void *p) {
  struct temp *t;
  for (t = temp_blocks; t; t = t->next)
    if ((void *)(t + 1) == p)
      return t;
  return NULL;
}

/* Tells if the system has temporary memory that it frees when the
   program quits (System 7). */
static int have_temp(void) {
  static int state; /* 0 not known, 1 yes, -1 no */
  long attr;
  if (!state)
    state = Gestalt(gestaltOSAttr, &attr) == noErr && (attr & TEMP_BITS) == TEMP_BITS ? 1 : -1;
  return state > 0;
}

/* Gives a block of n bytes from temporary memory, or NULL. */
static void *temp_alloc(size_t n) {
  OSErr err;
  Handle h;
  struct temp *t;
  if (n < TEMP_MIN || n > (size_t)0x7FFFFFFF - sizeof *t || !have_temp())
    return NULL;
  h = TempNewHandle((Size)(n + sizeof *t), &err);
  if (!h || err != noErr)
    return NULL;
  HLock(h);
  t = (struct temp *)*h;
  t->handle = h;
  t->size = n;
  t->next = temp_blocks;
  temp_blocks = t;
  return t + 1;
}

/* Releases a block from temporary memory if p is one; tells if it was. */
static int temp_free(void *p) {
  struct temp **l;
  for (l = &temp_blocks; *l; l = &(*l)->next)
    if ((void *)(*l + 1) == p) {
      Handle h = (*l)->handle;
      *l = (*l)->next;
      DisposeHandle(h);
      return 1;
    }
  return 0;
}

/* Gives a block of n bytes, or NULL with ENOMEM. */
void *malloc(size_t n) {
  void *p = NewPtr((Size)n);
  if (!p)
    p = temp_alloc(n);
  if (!p)
    errno = ENOMEM;
  return p;
}

/* Releases a block from malloc and friends. */
void free(void *p) {
  struct aligned *a;
  if (!p)
    return;
  if (temp_blocks && temp_free(p))
    return;
  a = aligned_blocks ? take_aligned(p) : NULL;
  DisposePtr(a ? a->block : (Ptr)p);
}

/* Gives a zeroed block for count objects of size bytes. */
void *calloc(size_t count, size_t size) {
  Ptr p;
  if (size && count > (size_t)0x7FFFFFFF / size) {
    errno = ENOMEM;
    return NULL;
  }
  p = NewPtrClear((Size)(count * size));
  if (!p && (p = temp_alloc(count * size)) != NULL)
    memset(p, 0, count * size);
  if (!p)
    errno = ENOMEM;
  return p;
}

/* Changes the size of a block; it can move. */
void *realloc(void *old, size_t n) {
  Ptr p;
  Size have;
  struct aligned *a;
  struct temp *t;
  if (!old)
    return malloc(n);
  if (temp_blocks && (t = find_temp(old)) != NULL) {
    /* A block of temporary memory moves to a new block. */
    void *q;
    if (n <= t->size)
      return old;
    q = malloc(n);
    if (!q)
      return NULL;
    memcpy(q, old, t->size);
    free(old);
    return q;
  }
  if (aligned_blocks && (a = take_aligned(old))) {
    /* Not in place: the block must stay aligned. */
    have = GetPtrSize(a->block) - ((char *)old - (char *)a->block);
    p = NewPtr((Size)n);
    if (!p) {
      a->next = aligned_blocks;   /* old stays valid */
      aligned_blocks = a;
      errno = ENOMEM;
      return NULL;
    }
    memcpy(p, old, have < (Size)n ? (size_t)have : n);
    DisposePtr(a->block);
    return p;
  }
  /* Grow or shrink in place when the heap allows it. */
  SetPtrSize((Ptr)old, (Size)n);
  if (MemError() == noErr)
    return old;
  p = malloc(n);
  if (!p)
    return NULL;
  have = GetPtrSize((Ptr)old);
  memcpy(p, old, have < (Size)n ? (size_t)have : n);
  DisposePtr((Ptr)old);
  return p;
}

/* Gives a block of n bytes at a multiple of align (a power of 2). */
void *aligned_alloc(size_t align, size_t n) {
  Ptr p;
  char *q;
  struct aligned *a;
  if (!align || (align & (align - 1))) {
    errno = EINVAL;
    return NULL;
  }
  p = malloc(n);
  if (!p || !((uintptr_t)p & (align - 1)))
    return p;
  DisposePtr(p);
  if (n > (size_t)0x7FFFFFFF - align - sizeof *a) {
    errno = ENOMEM;
    return NULL;
  }
  p = NewPtr((Size)(n + align + sizeof *a));
  if (!p) {
    errno = ENOMEM;
    return NULL;
  }
  q = (char *)(((uintptr_t)p + sizeof *a + align - 1) & ~(uintptr_t)(align - 1));
  a = (struct aligned *)q - 1;
  a->block = p;
  a->next = aligned_blocks;
  aligned_blocks = a;
  return q;
}

/* The same as aligned_alloc. */
void *memalign(size_t align, size_t n) {
  return aligned_alloc(align, n);
}

/* Puts an aligned block in *out; gives 0 or an error number. */
int posix_memalign(void **out, size_t align, size_t n) {
  void *p;
  int saved = errno;
  if (align % sizeof(void *) || (align & (align - 1)))
    return EINVAL;
  p = aligned_alloc(align, n);
  if (!p) {
    errno = saved;
    return ENOMEM;
  }
  *out = p;
  return 0;
}

/* Gives the number of bytes that the block p can hold. */
size_t malloc_usable_size(void *p) {
  struct aligned *a;
  struct temp *t;
  if (!p)
    return 0;
  if (temp_blocks && (t = find_temp(p)) != NULL)
    return t->size;
  for (a = aligned_blocks; a; a = a->next)
    if ((void *)(a + 1) == p)
      return (size_t)(GetPtrSize(a->block) - ((char *)p - (char *)a->block));
  return (size_t)GetPtrSize((Ptr)p);
}
