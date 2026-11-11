/* Console input: stdin reads the text file named after the program with
 * " Input" ("Hello Input" for Hello) in the program's folder, with each
 * Mac "\r" read as "\n". Without that file, stdin is at end of file.
 *
 * A program that reads its input elsewhere (a dialog, the serial port)
 * defines __mac_console_read itself; then this file is not linked.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <SegLoad.h>
#include "macos.h"

static short ref;
static int state;               /* 0 not open yet, 1 open, -1 none */

static void open_input(void) {
  Str255 name;
  const unsigned char *app = LMGetCurApName();
  static const char suffix[] = " Input";
  unsigned n = app[0], i;

  state = -1;
  if (n > 31 - (sizeof suffix - 1))
    n = 31 - (sizeof suffix - 1);
  for (i = 0; i < n; i++)
    name[1 + i] = app[1 + i];
  for (i = 0; i < sizeof suffix - 1; i++)
    name[1 + n + i] = (unsigned char)suffix[i];
  name[0] = (unsigned char)(n + sizeof suffix - 1);
  if (FSOpen(name, 0, &ref) == noErr)
    state = 1;
}

long __mac_console_read(void *buf, size_t n) {
  char *p = buf;
  long count = (long)n, i;
  OSErr err;
  if (state == 0)
    open_input();
  if (state < 0)
    return 0;
  err = FSRead(ref, &count, buf);
  if (err != noErr && err != eofErr)
    return -1;
  for (i = 0; i < count; i++)
    if (p[i] == '\r')
      p[i] = '\n';
  return count;
}
