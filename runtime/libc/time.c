/* gettimeofday from the Mac's clock, which counts local time in seconds
 * from 1904; time() and the rest of <time.h> use it. The time zone is the
 * one set in the Map control panel (its offset from UTC, daylight saving
 * time included when it is on). The first call reads it and, unless the
 * environment has been set, sets TZ to that fixed offset, so localtime
 * gives the Mac's local time once time() or gettimeofday has run. Times
 * in another season are off by the daylight saving hour, since the Mac
 * does not know the rules.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <stdlib.h>
#include <sys/time.h>
#include <OSUtils.h>

/* Seconds from 1904-01-01 to 1970-01-01. */
#define MAC_EPOCH 2082844800UL

extern char **environ;

/* Seconds east of UTC, once read. */
static long gmt_delta;
static int zone_read;

static char *two(char *s, unsigned v) {
  s[0] = (char)('0' + v / 10);
  s[1] = (char)('0' + v % 10);
  return s + 2;
}

static void read_zone(void) {
  /* "TZ=<+hhmm>-hh:mm:ss": POSIX counts the offset west of UTC. */
  static char tz[24] = "TZ=UTC0";
  static char *env[2] = { tz, NULL };
  MachineLocation loc;
  unsigned long off;
  char *s;
  zone_read = 1;
  ReadLocation(&loc);
  gmt_delta = loc.u.gmtDelta & 0xFFFFFF;
  if (gmt_delta & 0x800000)
    gmt_delta -= 0x1000000;
  if (environ && environ[0])
    return;                     /* something set the environment */
  if (gmt_delta) {
    off = (unsigned long)(gmt_delta < 0 ? -gmt_delta : gmt_delta);
    s = tz + 3;
    *s++ = '<';
    *s++ = gmt_delta < 0 ? '-' : '+';
    s = two(s, (unsigned)(off / 3600));
    s = two(s, (unsigned)(off / 60 % 60));
    *s++ = '>';
    *s++ = gmt_delta < 0 ? '+' : '-';
    s = two(s, (unsigned)(off / 3600));
    *s++ = ':';
    s = two(s, (unsigned)(off / 60 % 60));
    *s++ = ':';
    s = two(s, (unsigned)(off % 60));
    *s = 0;
  }
  environ = env;
}

int gettimeofday(struct timeval *restrict tv, void *restrict tz) {
  unsigned long secs;
  (void)tz;
  if (!zone_read)
    read_zone();
  GetDateTime(&secs);
  if (tv) {
    tv->tv_sec = (time_t)(secs - MAC_EPOCH) - gmt_delta;
    tv->tv_usec = 0;
  }
  return 0;
}
