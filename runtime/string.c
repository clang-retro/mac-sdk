/* The memory and string functions that clang calls on its own (memcpy,
 * memmove, memset for structure copies and zeroing, memcmp) and the
 * common <string.h> ones, for programs that have no C library.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Built with -ffreestanding -fno-builtin, so clang does not turn these
 * loops back into calls to themselves. */
#include <stddef.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  /* Word copies when both are even: the 68000 faults on odd word access. */
  if ((((unsigned long)d | (unsigned long)s) & 1) == 0) {
    for (; n >= 4; n -= 4, d += 4, s += 4)
      *(unsigned long *)d = *(const unsigned long *)s;
  }
  while (n--)
    *d++ = *s++;
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  if (d == s || n == 0)
    return dst;
  if (d < s || d >= s + n)
    return memcpy(dst, src, n);
  d += n;
  s += n;
  while (n--)
    *--d = *--s;
  return dst;
}

void *memset(void *dst, int c, size_t n) {
  unsigned char *d = dst;
  if (((unsigned long)d & 1) == 0 && n >= 4) {
    unsigned long w = (unsigned char)c;
    w |= w << 8;
    w |= w << 16;
    for (; n >= 4; n -= 4, d += 4)
      *(unsigned long *)d = w;
  }
  while (n--)
    *d++ = (unsigned char)c;
  return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *p = a, *q = b;
  for (; n; n--, p++, q++)
    if (*p != *q)
      return *p - *q;
  return 0;
}

void *memchr(const void *s, int c, size_t n) {
  const unsigned char *p = s;
  for (; n; n--, p++)
    if (*p == (unsigned char)c)
      return (void *)p;
  return NULL;
}

size_t strlen(const char *s) {
  const char *p = s;
  while (*p)
    p++;
  return p - s;
}

char *strcpy(char *restrict dst, const char *restrict src) {
  char *d = dst;
  while ((*d++ = *src++))
    ;
  return dst;
}

char *strncpy(char *restrict dst, const char *restrict src, size_t n) {
  char *d = dst;
  for (; n && *src; n--)
    *d++ = *src++;
  for (; n; n--)
    *d++ = 0;
  return dst;
}

char *strcat(char *restrict dst, const char *restrict src) {
  strcpy(dst + strlen(dst), src);
  return dst;
}

int strcmp(const char *a, const char *b) {
  for (; *a && *a == *b; a++, b++)
    ;
  return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  for (; n; n--, a++, b++) {
    if (*a != *b)
      return (unsigned char)*a - (unsigned char)*b;
    if (!*a)
      break;
  }
  return 0;
}

char *strchr(const char *s, int c) {
  for (;; s++) {
    if (*s == (char)c)
      return (char *)s;
    if (!*s)
      return NULL;
  }
}

char *strrchr(const char *s, int c) {
  const char *r = NULL;
  for (;; s++) {
    if (*s == (char)c)
      r = s;
    if (!*s)
      return (char *)r;
  }
}
