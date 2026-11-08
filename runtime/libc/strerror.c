/* picolibc asks for this (weakly) to name errors it does not know; the
 * Mac layer adds none.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>

char *_user_strerror(int errnum, int internal, int *errptr) {
  (void)errnum;
  (void)internal;
  (void)errptr;
  return 0;
}
