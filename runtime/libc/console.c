/* Console output: what a program writes to stdout and stderr goes to a
 * text file named after the program ("Hello Output" for Hello), made in
 * the program's folder on the first write, with each "\n" written as the
 * Mac's "\r" so that SimpleText shows the lines.
 *
 * A program that wants its output elsewhere (a window, the debugger)
 * defines __mac_console_write and __mac_console_close itself; then this
 * file is not linked.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <SegLoad.h>
#include "macos.h"

static short ref;
static int state;               /* 0 not open yet, 1 open, -1 failed */

static void open_console(void) {
  Str255 name;
  const unsigned char *app = LMGetCurApName();
  static const char suffix[] = " Output";
  unsigned n = app[0], i;
  OSErr err;

  state = -1;
  if (n > 31 - (sizeof suffix - 1))
    n = 31 - (sizeof suffix - 1);
  for (i = 0; i < n; i++)
    name[1 + i] = app[1 + i];
  for (i = 0; i < sizeof suffix - 1; i++)
    name[1 + n + i] = (unsigned char)suffix[i];
  name[0] = (unsigned char)(n + sizeof suffix - 1);
  err = Create(name, 0, 'ttxt', 'TEXT');
  if (err != noErr && err != dupFNErr)
    return;
  if (FSOpen(name, 0, &ref) != noErr)
    return;
  if (SetEOF(ref, 0) != noErr) {
    FSClose(ref);
    return;
  }
  state = 1;
}

void __mac_console_write(const void *buf, size_t n) {
  const char *p = buf;
  char chunk[128];
  if (state == 0)
    open_console();
  if (state < 0)
    return;
  while (n) {
    long k = 0;
    while (n && k < (long)sizeof chunk) {
      char c = *p++;
      chunk[k++] = c == '\n' ? '\r' : c;
      n--;
    }
    if (FSWrite(ref, &k, chunk) != noErr) {
      state = -1;
      return;
    }
  }
}

void __mac_console_close(void) {
  if (state == 1) {
    FSClose(ref);
    FlushVol(0, 0);
  }
  state = -1;
}
