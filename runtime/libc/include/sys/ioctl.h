/* sys/ioctl.h - ioctl, with the requests that the C library does:
 * FIONREAD (bytes that a read gets now), FIONBIO (O_NONBLOCK) and
 * SIOCATMARK (always 0). The numbers are as on Linux. It replaces
 * picolibc's header, and keeps its terminal requests.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef _SYS_IOCTL_H_
#define _SYS_IOCTL_H_

#include <sys/cdefs.h>
#include <sys/_types.h>

_BEGIN_STD_C

int ioctl(int fd, unsigned long op, void *param);

#define TIOCGWINSZ (('T' << 8) | 1)
#define TIOCSWINSZ (('T' << 8) | 2)
#define TIOCLINUX (('T' << 8) | 3)

#define FIONREAD 0x541B
#define FIONBIO 0x5421
#define SIOCATMARK 0x8905

_END_STD_C

#endif /* _SYS_IOCTL_H_ */
