/* agl.h - AGL, the Apple OpenGL interface to the Mac OS Window Manager
 * and QuickDraw: pixel formats, rendering contexts, drawables and
 * buffer swaps. It has the AGL 2.0 interface of Apple OpenGL 1.1 to 1.2
 * for the classic Mac OS.
 *
 * The functions are in the shared library OpenGLLibrary, with the gl*
 * functions. The linker imports them weakly: on a Mac without OpenGL
 * they are NULL. Examine (void *)aglChoosePixelFormat before the first
 * call. PowerPC only.
 *
 * Usual use:
 *
 *     GLint attr[] = {AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 16,
 *                     AGL_NONE};
 *     AGLPixelFormat fmt = aglChoosePixelFormat(NULL, 0, attr);
 *     AGLContext ctx = aglCreateContext(fmt, NULL);
 *     aglDestroyPixelFormat(fmt);
 *     aglSetDrawable(ctx, GetWindowPort(window));
 *     aglSetCurrentContext(ctx);
 *     ... draw with gl* ...
 *     aglSwapBuffers(ctx);
 *
 * Part of mac-sdk. Written from the AGL documentation; the values are
 * those of Apple's agl.h.
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#ifndef _AGL_H
#define _AGL_H

/* The Toolbox headers have no C++ linkage of their own. */
#ifdef __cplusplus
extern "C" {
#endif
#include <Quickdraw.h>
#ifdef __cplusplus
}
#endif
#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Weak on PowerPC, so that a program can test a function for NULL. On
 * 68k there is no OpenGL: a call does not link. */
#ifndef AGLAPI
#if defined(__powerpc__) || defined(__POWERPC__)
#define AGLAPI extern __attribute__((weak))
#else
#define AGLAPI extern
#endif
#endif

/* The version of this interface. */
#define AGL_VERSION_2_0 1

/* A graphics device, and a port that OpenGL draws in. */
typedef GDHandle AGLDevice;
typedef CGrafPtr AGLDrawable;

/* The opaque objects of AGL. */
typedef struct __AGLRendererInfoRec *AGLRendererInfo;
typedef struct __AGLPixelFormatRec *AGLPixelFormat;
typedef struct __AGLContextRec *AGLContext;

/* The attributes of aglChoosePixelFormat and aglDescribePixelFormat.
 * The list ends with AGL_NONE. An attribute followed by a number takes
 * that number; the others are true when they are in the list. */
#define AGL_NONE 0
#define AGL_ALL_RENDERERS 1    /* each renderer, also the non-compliant ones */
#define AGL_BUFFER_SIZE 2      /* the depth of a color index buffer */
#define AGL_LEVEL 3            /* the overlay (> 0) or underlay (< 0) level */
#define AGL_RGBA 4             /* RGBA, not color index */
#define AGL_DOUBLEBUFFER 5     /* a back buffer */
#define AGL_STEREO 6           /* left and right buffers */
#define AGL_AUX_BUFFERS 7      /* the number of auxiliary buffers */
#define AGL_RED_SIZE 8
#define AGL_GREEN_SIZE 9
#define AGL_BLUE_SIZE 10
#define AGL_ALPHA_SIZE 11
#define AGL_DEPTH_SIZE 12
#define AGL_STENCIL_SIZE 13
#define AGL_ACCUM_RED_SIZE 14
#define AGL_ACCUM_GREEN_SIZE 15
#define AGL_ACCUM_BLUE_SIZE 16
#define AGL_ACCUM_ALPHA_SIZE 17
#define AGL_PIXEL_SIZE 50      /* the bits per pixel of the frame buffer */
#define AGL_MINIMUM_POLICY 51  /* sizes are minimums */
#define AGL_MAXIMUM_POLICY 52  /* the largest sizes that the renderer has */
#define AGL_OFFSCREEN 53       /* draws in memory (aglSetOffScreen) */
#define AGL_FULLSCREEN 54      /* draws over the whole screen */
#define AGL_SAMPLE_BUFFERS_ARB 55
#define AGL_SAMPLES_ARB 56
#define AGL_AUX_DEPTH_STENCIL 57
#define AGL_RENDERER_ID 70     /* the ID of one renderer */
#define AGL_SINGLE_RENDERER 71 /* one renderer for all the screens */
#define AGL_NO_RECOVERY 72     /* no software renderer when the hardware fails */
#define AGL_ACCELERATED 73     /* hardware renderers only */
#define AGL_CLOSEST_POLICY 74  /* the color size nearest to the one asked */
#define AGL_ROBUST 75          /* renderers that do not fail */
#define AGL_BACKING_STORE 76   /* the back buffer stays after a swap */
#define AGL_MP_SAFE 78         /* safe for Multiprocessing Services tasks */
#define AGL_WINDOW 80          /* can draw in a window */
#define AGL_MULTISCREEN 81     /* one drawable over two or more screens */
#define AGL_VIRTUAL_SCREEN 82  /* (aglDescribePixelFormat only) */
#define AGL_COMPLIANT 83       /* renderers that comply with OpenGL */

/* The properties of aglDescribeRenderer. */
#define AGL_BUFFER_MODES 100
#define AGL_MIN_LEVEL 101
#define AGL_MAX_LEVEL 102
#define AGL_COLOR_MODES 103
#define AGL_ACCUM_MODES 104
#define AGL_DEPTH_MODES 105
#define AGL_STENCIL_MODES 106
#define AGL_MAX_AUX_BUFFERS 107
#define AGL_VIDEO_MEMORY 120
#define AGL_TEXTURE_MEMORY 121

/* The options of aglEnable, aglDisable, aglSetInteger and
 * aglGetInteger. */
#define AGL_SWAP_RECT 200          /* swap only this rectangle (4 values) */
#define AGL_BUFFER_RECT 202        /* draw in this part of the port */
#define AGL_SWAP_LIMIT 203         /* not more swaps than the screen refreshes */
#define AGL_COLORMAP_TRACKING 210  /* follow the palette in color index mode */
#define AGL_COLORMAP_ENTRY 212     /* set a color index entry (4 values) */
#define AGL_RASTERIZATION 220      /* false: no drawing (to time the code) */
#define AGL_SWAP_INTERVAL 222      /* the refreshes between swaps */
#define AGL_STATE_VALIDATION 230   /* check the state at each call */
#define AGL_BUFFER_NAME 231
#define AGL_ORDER_CONTEXT_TO_FRONT 232
#define AGL_CONTEXT_SURFACE_ID 233
#define AGL_CONTEXT_DISPLAY_ID 234
#define AGL_SURFACE_ORDER 235
#define AGL_SURFACE_OPACITY 236
#define AGL_CLIP_REGION 254        /* a region that clips the drawing */
#define AGL_FS_CAPTURE_SINGLE 255  /* full screen captures one screen only */

/* The options of aglConfigure. */
#define AGL_FORMAT_CACHE_SIZE 501  /* the pixel formats that AGL keeps */
#define AGL_CLEAR_FORMAT_CACHE 502 /* forget the kept pixel formats */
#define AGL_RETAIN_RENDERERS 503   /* keep the renderers loaded */

/* The buffer modes of AGL_BUFFER_MODES. */
#define AGL_MONOSCOPIC_BIT 0x00000001
#define AGL_STEREOSCOPIC_BIT 0x00000002
#define AGL_SINGLEBUFFER_BIT 0x00000004
#define AGL_DOUBLEBUFFER_BIT 0x00000008

/* The bit depths of AGL_DEPTH_MODES, AGL_STENCIL_MODES and the others. */
#define AGL_0_BIT 0x00000001
#define AGL_1_BIT 0x00000002
#define AGL_2_BIT 0x00000004
#define AGL_3_BIT 0x00000008
#define AGL_4_BIT 0x00000010
#define AGL_5_BIT 0x00000020
#define AGL_6_BIT 0x00000040
#define AGL_8_BIT 0x00000080
#define AGL_10_BIT 0x00000100
#define AGL_12_BIT 0x00000200
#define AGL_16_BIT 0x00000400
#define AGL_24_BIT 0x00000800
#define AGL_32_BIT 0x00001000
#define AGL_48_BIT 0x00002000
#define AGL_64_BIT 0x00004000
#define AGL_96_BIT 0x00008000
#define AGL_128_BIT 0x00010000

/* The color formats of AGL_COLOR_MODES and AGL_ACCUM_MODES. */
#define AGL_RGB8_BIT 0x00000001
#define AGL_RGB8_A8_BIT 0x00000002
#define AGL_BGR233_BIT 0x00000004
#define AGL_BGR233_A8_BIT 0x00000008
#define AGL_RGB332_BIT 0x00000010
#define AGL_RGB332_A8_BIT 0x00000020
#define AGL_RGB444_BIT 0x00000040
#define AGL_ARGB4444_BIT 0x00000080
#define AGL_RGB444_A8_BIT 0x00000100
#define AGL_RGB555_BIT 0x00000200
#define AGL_ARGB1555_BIT 0x00000400
#define AGL_RGB555_A8_BIT 0x00000800
#define AGL_RGB565_BIT 0x00001000
#define AGL_RGB565_A8_BIT 0x00002000
#define AGL_RGB888_BIT 0x00004000
#define AGL_ARGB8888_BIT 0x00008000
#define AGL_RGB888_A8_BIT 0x00010000
#define AGL_RGB101010_BIT 0x00020000
#define AGL_ARGB2101010_BIT 0x00040000
#define AGL_RGB101010_A8_BIT 0x00080000
#define AGL_RGB121212_BIT 0x00100000
#define AGL_ARGB12121212_BIT 0x00200000
#define AGL_RGB161616_BIT 0x00400000
#define AGL_ARGB16161616_BIT 0x00800000
#define AGL_INDEX8_BIT 0x20000000
#define AGL_INDEX16_BIT 0x40000000

/* The errors of aglGetError. */
#define AGL_NO_ERROR 0
#define AGL_BAD_ATTRIBUTE 10000
#define AGL_BAD_PROPERTY 10001
#define AGL_BAD_PIXELFMT 10002
#define AGL_BAD_RENDINFO 10003
#define AGL_BAD_CONTEXT 10004
#define AGL_BAD_DRAWABLE 10005
#define AGL_BAD_GDEV 10006
#define AGL_BAD_STATE 10007
#define AGL_BAD_VALUE 10008
#define AGL_BAD_MATCH 10009
#define AGL_BAD_ENUM 10010
#define AGL_BAD_OFFSCREEN 10011
#define AGL_BAD_FULLSCREEN 10012
#define AGL_BAD_WINDOW 10013
#define AGL_BAD_POINTER 10014
#define AGL_BAD_MODULE 10015
#define AGL_BAD_ALLOC 10016

/* Pixel formats. aglChoosePixelFormat finds the formats that have the
 * attributes in attribs, for the ndev devices in gdevs (all the screens
 * when gdevs is NULL). It gives NULL when there is none. */
AGLAPI AGLPixelFormat aglChoosePixelFormat(const AGLDevice *gdevs, GLint ndev,
                                    const GLint *attribs);
AGLAPI void aglDestroyPixelFormat(AGLPixelFormat pix);
AGLAPI AGLPixelFormat aglNextPixelFormat(AGLPixelFormat pix);
AGLAPI GLboolean aglDescribePixelFormat(AGLPixelFormat pix, GLint attrib,
                                 GLint *value);
AGLAPI AGLDevice *aglDevicesOfPixelFormat(AGLPixelFormat pix, GLint *ndevs);

/* The renderers, and what each can do. */
AGLAPI AGLRendererInfo aglQueryRendererInfo(const AGLDevice *gdevs, GLint ndev);
AGLAPI void aglDestroyRendererInfo(AGLRendererInfo rend);
AGLAPI AGLRendererInfo aglNextRendererInfo(AGLRendererInfo rend);
AGLAPI GLboolean aglDescribeRenderer(AGLRendererInfo rend, GLint prop,
                              GLint *value);

/* Rendering contexts. share, when not NULL, shares its display lists
 * and textures with the new context. */
AGLAPI AGLContext aglCreateContext(AGLPixelFormat pix, AGLContext share);
AGLAPI GLboolean aglDestroyContext(AGLContext ctx);
AGLAPI GLboolean aglCopyContext(AGLContext src, AGLContext dst, GLuint mask);
/* Call it after the window moves or changes size. */
AGLAPI GLboolean aglUpdateContext(AGLContext ctx);

/* The current context of the program. */
AGLAPI GLboolean aglSetCurrentContext(AGLContext ctx);
AGLAPI AGLContext aglGetCurrentContext(void);

/* Drawables. aglSetDrawable(ctx, NULL) detaches the context. */
AGLAPI GLboolean aglSetDrawable(AGLContext ctx, AGLDrawable draw);
AGLAPI GLboolean aglSetOffScreen(AGLContext ctx, GLsizei width, GLsizei height,
                          GLsizei rowbytes, GLvoid *baseaddr);
AGLAPI GLboolean aglSetFullScreen(AGLContext ctx, GLsizei width, GLsizei height,
                           GLsizei freq, GLint device);
AGLAPI AGLDrawable aglGetDrawable(AGLContext ctx);

/* The virtual screen (the renderer) of a context. */
AGLAPI GLboolean aglSetVirtualScreen(AGLContext ctx, GLint screen);
AGLAPI GLint aglGetVirtualScreen(AGLContext ctx);

/* The version of the library, and its global options. */
AGLAPI void aglGetVersion(GLint *major, GLint *minor);
AGLAPI GLboolean aglConfigure(GLenum pname, GLuint param);

/* Shows the back buffer, and flushes the commands. */
AGLAPI void aglSwapBuffers(AGLContext ctx);

/* The options of a context (AGL_SWAP_RECT and the others). */
AGLAPI GLboolean aglEnable(AGLContext ctx, GLenum pname);
AGLAPI GLboolean aglDisable(AGLContext ctx, GLenum pname);
AGLAPI GLboolean aglIsEnabled(AGLContext ctx, GLenum pname);
AGLAPI GLboolean aglSetInteger(AGLContext ctx, GLenum pname, const GLint *params);
AGLAPI GLboolean aglGetInteger(AGLContext ctx, GLenum pname, GLint *params);

/* Makes display lists base to base + count - 1 of the characters first
 * to first + count - 1 of a Font Manager font. */
AGLAPI GLboolean aglUseFont(AGLContext ctx, GLint fontID, Style face, GLint size,
                     GLint first, GLint count, GLint base);

/* The last error, and its text. */
AGLAPI GLenum aglGetError(void);
AGLAPI const GLubyte *aglErrorString(GLenum code);

/* Unloads the renderers and frees what AGL keeps. */
AGLAPI void aglResetLibrary(void);

#ifdef __cplusplus
}
#endif

#endif /* _AGL_H */
