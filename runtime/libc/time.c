/* gettimeofday from the Mac's clock, which counts local time in seconds
 * from 1904; time() and the rest of <time.h> use it. The time zone is the
 * one set in the Map control panel (its offset from UTC, daylight saving
 * time included when it is on). The first call reads it and, unless the
 * environment has been set, sets TZ to that fixed offset, so localtime
 * gives the Mac's local time once time() or gettimeofday has run. Times
 * in another season are off by the daylight saving hour, since the Mac
 * does not know the rules.
 *
 * CLOCK_MONOTONIC and the fractions of a second of gettimeofday come from
 * a fine clock (__mac_microseconds): UpTime on PowerPC when
 * DriverServicesLib has it, else Microseconds (System 7 and later on
 * 68k, all PowerPC Macs), else TickCount (1/60 second, System 6).
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
#include <Timer.h>
#if TARGET_CPU_PPC
#include <CodeFragments.h>
#include <DriverServices.h>
#endif
#include "macos.h"

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

/* The trap word of Microseconds. */
#define MICROSECONDS_TRAP 0xA193

/* Which fine clock __mac_microseconds reads: FINE_UNKNOWN before the
   first call, then one of the other values. */
enum { FINE_UNKNOWN, FINE_UPTIME, FINE_MICROSECONDS, FINE_TICKS };
static int fine_clock;

#if TARGET_CPU_PPC
/* AbsoluteToNanoseconds of DriverServicesLib, which the headers do not
   declare. On PowerPC a structure result goes to a hidden pointer, as
   the Mac's libraries expect. */
typedef Nanoseconds (*abs_to_ns_fn)(AbsoluteTime t);
static __typeof__(UpTime) *up_time;
static abs_to_ns_fn abs_to_ns;

/* Finds UpTime and AbsoluteToNanoseconds in DriverServicesLib. Gives 1
   when both are there. NuBus Power Macs before Mac OS 7.5.2 do not have
   the library. The program does not import it, so that it loads on
   those Macs too. */
static int find_uptime(void) {
  ConnectionID conn;
  Ptr entry, sym;
  SymClass cls;
  Str255 err;
  if (GetSharedLibrary("\pDriverServicesLib", kPowerPCCFragArch, kReferenceCFrag,
                       &conn, &entry, err) != noErr)
    return 0;
  if (FindSymbol(conn, "\pUpTime", &sym, &cls) != noErr)
    return 0;
  up_time = (__typeof__(UpTime) *)(void *)sym;
  if (FindSymbol(conn, "\pAbsoluteToNanoseconds", &sym, &cls) != noErr)
    return 0;
  abs_to_ns = (abs_to_ns_fn)(void *)sym;
  return 1;
}
#endif

/* Selects the finest clock that this Mac has, one time. */
static void select_fine_clock(void) {
#if TARGET_CPU_PPC
  fine_clock = find_uptime() ? FINE_UPTIME : FINE_MICROSECONDS;
#else
  fine_clock = TrapAvailable(MICROSECONDS_TRAP) ? FINE_MICROSECONDS : FINE_TICKS;
#endif
}

/* Gives the microseconds since the Mac started. The value never goes
   back. Its step is 1 microsecond with UpTime or Microseconds, and 1/60
   second (16667 microseconds) on a Mac that has neither. */
unsigned long long __mac_microseconds(void) {
  UnsignedWide w;
  if (fine_clock == FINE_UNKNOWN)
    select_fine_clock();
#if TARGET_CPU_PPC
  if (fine_clock == FINE_UPTIME) {
    Nanoseconds ns = abs_to_ns(up_time());
    return (((unsigned long long)ns.hi << 32) | ns.lo) / 1000;
  }
#endif
  if (fine_clock == FINE_MICROSECONDS) {
    Microseconds(&w);
    return ((unsigned long long)w.hi << 32) | w.lo;
  }
  return (unsigned long long)TickCount() * 50000 / 3;
}

/* Tells if the fine clock steps in microseconds (1), or in ticks (0). */
int __mac_microseconds_fine(void) {
  if (fine_clock == FINE_UNKNOWN)
    select_fine_clock();
  return fine_clock != FINE_TICKS;
}

/* The Mac's clock counts whole seconds. gettimeofday adds the time of
   the fine clock since the clock was last read as the base, so that the
   time moves in steps of a microsecond (of a tick without a fine clock).
   The base moves forward when the clock is more than a second ahead of
   the sum; the time never goes back. */
static unsigned long base_secs;
static unsigned long long base_us;
static int have_base;

/* Gives the time, with microseconds for the fractions of a second. */
int gettimeofday(struct timeval *restrict tv, void *restrict tz) {
  unsigned long secs;
  unsigned long long us, elapsed;
  (void)tz;
  if (!zone_read)
    read_zone();
  GetDateTime(&secs);
  us = __mac_microseconds();
  if (!have_base || secs > base_secs + (us - base_us) / 1000000 + 1) {
    base_secs = secs;
    base_us = us;
    have_base = 1;
  }
  elapsed = us - base_us;
  if (tv) {
    tv->tv_sec = (time_t)(base_secs + (unsigned long)(elapsed / 1000000) - MAC_EPOCH) -
                 gmt_delta;
    tv->tv_usec = (suseconds_t)(elapsed % 1000000);
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

/* The time of a clock: CLOCK_REALTIME is the Mac's clock in UTC, with
   the fine clock for the fractions of a second; CLOCK_MONOTONIC is the
   fine clock, from the start of the Mac. */
int clock_gettime(clockid_t clock_id, struct timespec *tp) {
  struct timeval tv;
  unsigned long long us;
  if (clock_id == CLOCK_MONOTONIC) {
    us = __mac_microseconds();
    tp->tv_sec = (time_t)(us / 1000000);
    tp->tv_nsec = (long)(us % 1000000) * 1000L;
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

/* Gives the step of a clock in *res (when res is not NULL): 1
   microsecond with a fine clock, else 1/60 second. Gives 0, or -1 with
   errno set to EINVAL for an unknown clock. */
int clock_getres(clockid_t clock_id, struct timespec *res) {
  if (clock_id != CLOCK_MONOTONIC && clock_id != CLOCK_REALTIME) {
    errno = EINVAL;
    return -1;
  }
  if (res) {
    res->tv_sec = 0;
    res->tv_nsec = __mac_microseconds_fine() ? 1000L : 16666667L;
  }
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
