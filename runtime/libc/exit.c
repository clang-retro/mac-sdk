/* Startup and exit. start.S (assembled with -DMAC_LIBC) and the PowerPC
 * entry point call __crt_main, which runs main with the program's name as
 * argv[0], after the static constructors, and passes its result to exit, so that stdio is flushed and
 * atexit functions run. _exit closes the console file and quits to the
 * Finder.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <stdlib.h>
#include <unistd.h>
#include <SegLoad.h>
#include "macos.h"

int main(int argc, char **argv);
void __crt_main(void);
void __libc_init_array(void);

void __crt_main(void) {
  static char name[32];
  static char *argv[2] = { name, NULL };
  const unsigned char *app = LMGetCurApName();
  unsigned i, n = app[0] > 31 ? 31 : app[0];
  for (i = 0; i < n; i++)
    name[i] = (char)app[1 + i];
  name[n] = 0;
  __libc_init_array();          /* static constructors */
  exit(main(1, argv));
}

void _exit(int status) {
  (void)status;
  __mac_console_close();
  ExitToShell();
  for (;;)
    ;
}
