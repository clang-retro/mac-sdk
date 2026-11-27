/* Checks the OpenGL headers and imports: AGL pixel formats and
 * contexts, an off-screen drawable, glClear and glReadPixels, the
 * strings, GLU, FindSymbol, and that the functions are weak imports
 * (not NULL when OpenGL is there). PowerPC only. tests/run.sh runs it in
 * tools/simppc.py, whose fake OpenGL (tools/fakegl.py) keeps the
 * off-screen buffer, and checks the console output: one "ok" line per
 * check, then "all ok".
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <AGL/agl.h>
#include <CodeFragments.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 8
#define H 4

static int failures;

/* Prints one check's result, and counts the failures. */
static void check(const char *what, int ok) {
  printf("%s %s\n", ok ? "ok" : "FAILED", what);
  if (!ok)
    failures++;
}

/* Makes an off-screen context, clears it and reads the pixels back. */
static void check_offscreen(void) {
  GLint attr[] = {AGL_RGBA, AGL_OFFSCREEN, AGL_PIXEL_SIZE, 32, AGL_DEPTH_SIZE, 16, AGL_NONE};
  GLint value = 0, vp[4];
  unsigned char px[4 * W * H];
  unsigned long *buf = calloc(W * H, 4);
  AGLPixelFormat fmt = aglChoosePixelFormat(NULL, 0, attr);
  AGLContext ctx;
  const char *ext;
  check("aglChoosePixelFormat", fmt != NULL);
  check("aglDescribePixelFormat", aglDescribePixelFormat(fmt, AGL_DEPTH_SIZE, &value) && value == 16);
  ctx = aglCreateContext(fmt, NULL);
  aglDestroyPixelFormat(fmt);
  check("aglCreateContext", ctx != NULL);
  check("aglSetOffScreen", aglSetOffScreen(ctx, W, H, 4 * W, buf));
  check("aglSetCurrentContext", aglSetCurrentContext(ctx) && aglGetCurrentContext() == ctx);
  ext = (const char *)glGetString(GL_EXTENSIONS);
  check("glGetString", glGetString(GL_VERSION) != NULL && ext != NULL);
  check("gluCheckExtension",
        gluCheckExtension((const GLubyte *)"GL_EXT_bgra", (const GLubyte *)ext));
  glViewport(0, 0, W, H);
  glGetIntegerv(GL_VIEWPORT, vp);
  check("glViewport", vp[0] == 0 && vp[1] == 0 && vp[2] == W && vp[3] == H);
  glClearColor(1.0f, 0.5f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPerspective(45.0, (double)W / H, 0.1, 10.0);
  glBegin(GL_TRIANGLES);
  glColor3f(1, 1, 1);
  glVertex3f(0, 0, -1);
  glVertex3f(1, 0, -1);
  glVertex3f(0, 1, -1);
  glEnd();
  glFinish();
  /* The buffer has 32-bit ARGB pixels. */
  check("glClear", buf[0] == 0xFFFF8000UL && buf[W * H - 1] == 0xFFFF8000UL);
  memset(px, 0, sizeof px);
  glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
  check("glReadPixels", px[0] == 255 && px[1] == 128 && px[2] == 0 && px[3] == 255);
  glReadPixels(0, 0, 1, 1, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
  check("glReadPixels BGRA", px[0] == 0 && px[1] == 128 && px[2] == 255);
  check("glGetError", glGetError() == GL_NO_ERROR);
  aglSwapBuffers(ctx);
  check("aglSetCurrentContext NULL", aglSetCurrentContext(NULL) && aglGetCurrentContext() == NULL);
  check("aglDestroyContext", aglDestroyContext(ctx));
  free(buf);
}

/* Finds a function of OpenGLLibrary by its name, as a program does for
 * an extension, with the Code Fragment Manager. */
static void check_find_symbol(void) {
  ConnectionID conn = 0;
  Ptr entry = 0, addr = 0;
  SymClass cls = 0;
  Str255 err;
  OSErr e = GetSharedLibrary("\pOpenGLLibrary", kPowerPCArch, kReferenceCFrag, &conn, &entry, err);
  check("GetSharedLibrary", e == noErr);
  check("FindSymbol", e == noErr && FindSymbol(conn, "\pglFlush", &addr, &cls) == noErr && addr);
  if (addr)
    ((void (*)(void))addr)();
  // OpenGLLibrary has no function by this name.
  Ptr none = 0;
  check("FindSymbol of a missing name",
        e == noErr && FindSymbol(conn, "\pglNoSuchFunctionXYZ", &none, &cls) == cfragNoSymbolErr);
}

/* Runs the checks. */
int main(void) {
  GLint major = 0, minor = 0;
  /* Weak imports: NULL on a Mac without OpenGL. */
  if ((void *)aglChoosePixelFormat == NULL || (void *)glBegin == NULL) {
    printf("no OpenGL\n");
    return 0;
  }
  check("OpenGLUtility", (void *)gluPerspective != NULL);
  aglGetVersion(&major, &minor);
  check("aglGetVersion", major >= 2);
  check("gluErrorString", gluErrorString(GLU_INVALID_ENUM) != NULL);
  check("gluGetString", gluGetString(GLU_VERSION) != NULL);
  check_offscreen();
  check_find_symbol();
  printf(failures ? "%d FAILED\n" : "all ok\n", failures);
  return failures != 0;
}
