/* The console as a window: what a program writes to stdout and stderr
 * shows in a window in Monaco 9, and stdin reads the lines typed into it.
 * tools/macapp.py links it in place of the console file (console.c and
 * input.c) with --console window.
 *
 * The window opens on the first output or input. It initializes the
 * Toolbox first, unless the program sets _console_toolbox to 0 before
 * then because it has done that itself. Text wraps at the window's
 * width, and the window scrolls up a line when it is full; it keeps no
 * history above that. Typing waits for Return, with Delete to take back a
 * character; Command-D or Command-period, or a click in the close box,
 * is the end of input. When the program ends, the window stays until a
 * key is pressed or the window is clicked, so its output can be read.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <stdio.h>
#include <Dialogs.h>
#include <Events.h>
#include <Fonts.h>
#include <Menus.h>
#include <OSUtils.h>
#include <Quickdraw.h>
#include <SegLoad.h>
#include <TextEdit.h>
#include <Windows.h>
#include "../macos.h"

#define MAXCOLS 80
#define MAXROWS 24
#define MARGIN 4

int _console_toolbox = 1;

static QDGlobals console_qd;
static WindowPtr win;
static RgnHandle scrolled;      /* what ScrollRect uncovers */
static int state;               /* 0 not open yet, 1 open, -1 ended */
static int cols, rows, row, col, height, ascent, width;
static char grid[MAXROWS][MAXCOLS];
static char line[256];          /* typed, not yet read */
static int have, given, eof;

static void draw_row(int r) {
  Rect box;
  SetRect(&box, 0, MARGIN + r * height, MARGIN * 2 + cols * width,
          MARGIN + (r + 1) * height);
  EraseRect(&box);
  MoveTo(MARGIN, MARGIN + r * height + ascent);
  DrawText(grid[r], 0, (short)cols);
}

static void draw_all(void) {
  int r;
  for (r = 0; r < rows; r++)
    draw_row(r);
}

static void open_window(void) {
  Rect bounds, screen;
  FontInfo fi;
  int i;
  state = -1;
  if (_console_toolbox) {
    InitGraf(&console_qd.thePort);
    InitFonts();
    FlushEvents(everyEvent, 0);
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    screen = console_qd.screenBits.bounds;
  } else {
    /* The program's own QuickDraw globals: the screen is GrayRgn's box. */
    screen = (**GetGrayRgn()).rgnBBox;
  }
  if (screen.right - screen.left < 200 || screen.bottom - screen.top < 150)
    SetRect(&screen, 0, 0, 512, 342);   /* a Mac Plus */
  SetRect(&bounds, 0, 0, 10, 10);
  win = NewWindow(NULL, &bounds, LMGetCurApName(), false, noGrowDocProc,
                  (WindowPtr)-1, true, 0);
  scrolled = NewRgn();
  if (!win || !scrolled)
    return;
  SetPort(win);
  TextFont(4);                  /* Monaco */
  TextSize(9);
  GetFontInfo(&fi);
  ascent = fi.ascent;
  height = fi.ascent + fi.descent + fi.leading;
  width = fi.widMax;
  if (height <= 0 || width <= 0) {
    ascent = 9;
    height = 11;
    width = 6;
  }
  cols = (screen.right - screen.left - 16 - 2 * MARGIN) / width;
  rows = (screen.bottom - screen.top - 60 - 2 * MARGIN) / height;
  if (cols > MAXCOLS)
    cols = MAXCOLS;
  if (rows > MAXROWS)
    rows = MAXROWS;
  for (i = 0; i < MAXROWS; i++) {
    int j;
    for (j = 0; j < MAXCOLS; j++)
      grid[i][j] = ' ';
  }
  SizeWindow(win, (short)(cols * width + 2 * MARGIN),
             (short)(rows * height + 2 * MARGIN), true);
  MoveWindow(win, (short)(screen.left + 8), (short)(screen.top + 44), true);
  ShowWindow(win);
  state = 1;
}

static void newline(void) {
  col = 0;
  if (row + 1 < rows) {
    row++;
    return;
  }
  /* Full: move every row up one, on the screen too. */
  {
    int r, j;
    Rect box;
    for (r = 0; r + 1 < rows; r++)
      for (j = 0; j < cols; j++)
        grid[r][j] = grid[r + 1][j];
    for (j = 0; j < cols; j++)
      grid[rows - 1][j] = ' ';
    SetRect(&box, 0, MARGIN, MARGIN * 2 + cols * width,
            MARGIN + rows * height);
    ScrollRect(&box, 0, (short)-height, scrolled);
    draw_row(rows - 1);
  }
}

/* Put n characters on the screen, drawing each run on a row at once. */
static void put(const char *p, size_t n) {
  while (n) {
    int start = col, first = row;
    while (n && *p != '\n' && *p != '\r' && *p != '\b' && *p != '\t' &&
           col < cols) {
      grid[row][col++] = *p++;
      n--;
    }
    if (col > start) {
      MoveTo((short)(MARGIN + start * width),
             (short)(MARGIN + first * height + ascent));
      DrawText(grid[first], (short)start, (short)(col - start));
    }
    if (!n)
      break;
    if (col >= cols) {
      newline();
      continue;
    }
    if (*p == '\n' || *p == '\r') {
      newline();
    } else if (*p == '\b') {
      if (col > 0) {
        col--;
        grid[row][col] = ' ';
        draw_row(row);
      }
    } else {                    /* tab: to the next multiple of 8 */
      do
        grid[row][col++] = ' ';
      while (col % 8 && col < cols);
    }
    p++;
    n--;
  }
}

static void update(EventRecord *ev) {
  WindowPtr w = (WindowPtr)ev->message;
  BeginUpdate(w);
  if (w == win)
    draw_all();
  EndUpdate(w);
}

/* Waits for one key (or the end of input): its character, or -1. */
static int next_key(void) {
  EventRecord ev;
  WindowPtr w;
  for (;;) {
    SystemTask();
    if (!GetNextEvent(everyEvent, &ev))
      continue;
    switch (ev.what) {
    case keyDown:
    case autoKey: {
      char c = (char)(ev.message & charCodeMask);
      if ((ev.modifiers & cmdKey) && (c == '.' || c == 'd' || c == 'D'))
        return -1;
      if (!(ev.modifiers & cmdKey))
        return (unsigned char)c;
      break;
    }
    case updateEvt:
      update(&ev);
      break;
    case mouseDown:
      switch (FindWindow(ev.where, &w)) {
      case inSysWindow:
        SystemClick(&ev, w);
        break;
      case inDrag:
        DragWindow(w, ev.where, &(**GetGrayRgn()).rgnBBox);
        break;
      case inGoAway:
        if (w == win && TrackGoAway(w, ev.where))
          return -1;
        break;
      case inContent:
        if (w == win && state < 0)
          return -1;            /* the program has ended */
        SelectWindow(w);
        break;
      }
      break;
    }
  }
}

void __mac_console_write(const void *buf, size_t n) {
  if (state == 0)
    open_window();
  if (state > 0)
    put(buf, n);
}

long __mac_console_read(void *buf, size_t n) {
  char *out = buf;
  long k = 0;
  fflush(stdout);               /* show a prompt that has no newline */
  if (state == 0)
    open_window();
  if (state <= 0)
    return 0;
  if (given == have && !eof) {
    have = given = 0;
    for (;;) {
      int c = next_key();
      if (c < 0) {
        eof = 1;
        break;
      }
      if (c == '\b') {          /* Delete */
        if (have) {
          have--;
          put("\b", 1);
        }
        continue;
      }
      if (c == '\r' || c == '\n') {
        line[have++] = '\n';
        put("\n", 1);
        break;
      }
      if (have < (int)sizeof line - 1) {
        char ch = (char)c;
        line[have++] = ch;
        put(&ch, 1);
      }
    }
  }
  while (given < have && k < (long)n)
    out[k++] = line[given++];
  return k;
}

void __mac_console_close(void) {
  static const char done[] = "\n[Finished: press a key or click]";
  if (state > 0) {
    put(done, sizeof done - 1);
    state = -1;
    eof = 0;
    next_key();
    DisposeWindow(win);
  }
  state = -1;
}
