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
#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/timeb.h>
#include <time.h>
#include <OSUtils.h>
#include <Events.h>

/* Seconds from 1904-01-01 to 1970-01-01. */
#define MAC_EPOCH 2082844800UL

/* time.h gives CLOCK_MONOTONIC only with _GNU_SOURCE. */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 4
#endif

extern char **environ;

/* Seconds east of UTC, once read. */
static long gmt_delta;
static int zone_read;
/* 1 when the Map control panel says daylight saving time is on. */
static int dst_on;

/* Writes a number from 0 to 99 as two digits, and gives the next position. */
static char *two(char *s, unsigned v) {
  s[0] = (char)('0' + v / 10);
  s[1] = (char)('0' + v % 10);
  return s + 2;
}

/* Reads the time zone from the location in parameter RAM, one time. */
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
  dst_on = (loc.u.dlsDelta & 0x80) != 0;
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

/* The Mac's clock counts whole seconds. gettimeofday adds the ticks
   (1/60 second) since the clock was last read as the base, so that the
   time moves in steps of a tick, as timed waits need. The base moves
   forward when the clock is more than a second ahead of the sum; the
   time never goes back. */
static unsigned long base_secs, base_ticks;
static int have_base;

/* Gives the time, with ticks for the fractions of a second. */
int gettimeofday(struct timeval *restrict tv, void *restrict tz) {
  unsigned long secs, ticks, elapsed;
  (void)tz;
  if (!zone_read)
    read_zone();
  GetDateTime(&secs);
  ticks = TickCount();
  if (!have_base || secs > base_secs + (ticks - base_ticks) / 60 + 1) {
    base_secs = secs;
    base_ticks = ticks;
    have_base = 1;
  }
  elapsed = ticks - base_ticks;
  if (tv) {
    tv->tv_sec = (time_t)(base_secs + elapsed / 60 - MAC_EPOCH) - gmt_delta;
    tv->tv_usec = (suseconds_t)((elapsed % 60) * 1000000UL / 60);
  }
  return 0;
}

/* Seconds east of UTC of the Mac's clock (the Map control panel), for
   the file dates of stat.c. */
long __mac_gmt_delta(void) {
  if (!zone_read)
    read_zone();
  return gmt_delta;
}

/* The time of a clock: CLOCK_REALTIME is the Mac's clock in UTC, to the
   second; CLOCK_MONOTONIC counts from the start of the Mac in ticks (60
   per second). */
int clock_gettime(clockid_t clock_id, struct timespec *tp) {
  struct timeval tv;
  unsigned long ticks;
  if (clock_id == CLOCK_MONOTONIC) {
    ticks = TickCount();
    tp->tv_sec = (time_t)(ticks / 60);
    tp->tv_nsec = (long)(ticks % 60) * 16666667L;
    return 0;
  }
  if (clock_id != CLOCK_REALTIME) {
    errno = EINVAL;
    return -1;
  }
  gettimeofday(&tv, NULL);
  tp->tv_sec = tv.tv_sec;
  tp->tv_nsec = (long)tv.tv_usec * 1000;
  return 0;
}

/* Gives the time in *tb: the seconds since 1970 (UTC) and the
   milliseconds, the time zone of the Map control panel in minutes west
   of UTC (standard time), and 1 in dstflag when daylight saving time is
   on. gmt_delta includes the daylight saving hour, so it is taken out of
   timezone. Gives 0. */
int ftime(struct timeb *tb) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  tb->time = tv.tv_sec;
  tb->millitm = (unsigned short)(tv.tv_usec / 1000);
  tb->timezone = (short)(-gmt_delta / 60 + (dst_on ? 60 : 0));
  tb->dstflag = (short)dst_on;
  return 0;
}
