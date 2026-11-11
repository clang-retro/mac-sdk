/* Checks the console window (tools/macapp.py --console window): output
 * with wrapping, tabs and scrolling, and lines typed for stdin.
 * tests/run.sh runs it in tools/sim68k.py --keys and tools/simppc.py
 * --keys, typing "Ada\n" and then Command-period (end of input), and
 * looks for the text it draws.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <stdio.h>
#include <string.h>

int main(void) {
  char buf[64];
  int i, ok;
  printf("Name? ");
  ok = fgets(buf, sizeof buf, stdin) && strcmp(buf, "Ada\n") == 0;
  printf("Hello, %s", buf);
  for (i = 0; i < 30; i++)          /* more than the window's rows */
    printf("line %d\tend\n", i);
  ok = ok && !fgets(buf, sizeof buf, stdin) && feof(stdin);
  printf(ok ? "console ok\n" : "console FAILED\n");
  return 0;
}
