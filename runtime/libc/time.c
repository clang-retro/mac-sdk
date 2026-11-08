/* gettimeofday from the Mac's clock, which counts local time in seconds
 * from 1904; time() and the rest of <time.h> use it. There are no time
 * zones, so local time is reported as UTC.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <sys/time.h>
#include <OSUtils.h>

/* Seconds from 1904-01-01 to 1970-01-01. */
#define MAC_EPOCH 2082844800UL

int gettimeofday(struct timeval *restrict tv, void *restrict tz) {
  unsigned long secs;
  (void)tz;
  GetDateTime(&secs);
  if (tv) {
    tv->tv_sec = (time_t)(secs - MAC_EPOCH);
    tv->tv_usec = 0;
  }
  return 0;
}
