/* The classic Mac OS layer under picolibc: what the C library's file,
 * console, memory and time functions call, written with this SDK's
 * Toolbox headers.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef MACOS_LIBC_H
#define MACOS_LIBC_H

#include <stddef.h>
#include <Files.h>

/* errno for a File Manager or Memory Manager result. */
int __mac_errno(OSErr err);

/* A POSIX or Mac path as a Pascal string: "a/b" is ":a:b", "/Disk/a" is
 * "Disk:a", and a name without "/" is used as it is (a Mac path). Returns
 * 0, or -1 with errno set when the path is too long. */
int __mac_path(const char *path, Str255 out);

/* Console output (stdout and stderr). */
void __mac_console_write(const void *buf, size_t n);
void __mac_console_close(void);

/* Console input (stdin): up to n bytes, 0 at end of file, or -1. */
long __mac_console_read(void *buf, size_t n);

#endif
