# OpenGL

PowerPC programs can use Apple OpenGL for the classic Mac OS. The SDK
has the headers and the links to Apple's shared libraries. It does not
have an OpenGL: the program uses the OpenGL that is on the Mac.

## Summary

| Header | Gives | Shared library |
| --- | --- | --- |
| `GL/gl.h` | OpenGL 1.0, 1.1 and 1.2 | `OpenGLLibrary` |
| `GL/glext.h` | The extensions that Apple OpenGL exports (see below) | `OpenGLLibrary` |
| `GL/glu.h` | GLU 1.3 | `OpenGLUtility` |
| `AGL/agl.h` | AGL 2.0: pixel formats, contexts, drawables, swaps | `OpenGLLibrary` |

`GL/gl.h` includes `GL/glext.h`. `AGL/agl.h` includes `Quickdraw.h`
and `GL/gl.h`.

## Requirements

- A PowerPC Mac with Apple OpenGL 1.1 or later. Mac OS 9 has it.
  For Mac OS 8.1 to 8.6, the user installs Apple's OpenGL installer.
- Apple OpenGL 1.1 has GLU 1.2. The GLU 1.3 functions
  (`gluBuild3DMipmaps`, `gluCheckExtension` and the others) can be
  missing there.
- There is no OpenGL for 68k Macs. On 68k, a call to a `gl`, `glu` or
  `agl` function does not link. In a fat application, put the OpenGL
  code in `#if defined(__POWERPC__)`.

## Weak imports

The linker (`tools/link/peflink.py`) imports `OpenGLLibrary` and
`OpenGLUtility` weakly. The headers declare each function with
`__attribute__((weak))` on PowerPC. On a Mac without OpenGL, the
program starts and each OpenGL function is `NULL`. Examine one before
the first call:

```c
if ((void *)aglChoosePixelFormat == NULL) {
  /* No OpenGL on this Mac. */
}
```

A function that the installed version does not have is `NULL` too.

## A context in a window

```c
#include <AGL/agl.h>

GLint attr[] = {AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE, 16, AGL_NONE};
AGLPixelFormat fmt = aglChoosePixelFormat(NULL, 0, attr);
AGLContext ctx = aglCreateContext(fmt, NULL);
aglDestroyPixelFormat(fmt);
aglSetDrawable(ctx, (AGLDrawable)window);   /* the window's port */
aglSetCurrentContext(ctx);
glClearColor(0, 0, 0, 1);
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
/* ... draw ... */
aglSwapBuffers(ctx);
```

- After the window moves or changes size, call `aglUpdateContext`.
- `aglSetOffScreen(ctx, width, height, rowbytes, base)` draws in memory
  instead. Ask for `AGL_OFFSCREEN` in the pixel format. The pixels are
  32-bit ARGB, top row first.
- `aglSetDrawable(ctx, NULL)` detaches the context. Then call
  `aglDestroyContext`.
- `aglUseFont` makes display lists from a Font Manager font.

## Extensions

The classic Mac OS has no `aglGetProcAddress`. Apple OpenGL exports the
functions of its extensions by name. `GL/glext.h` declares these:

- `GL_ARB_multitexture`, `GL_ARB_transpose_matrix`,
  `GL_ARB_texture_cube_map`, `GL_ARB_texture_env_add`
- `GL_EXT_compiled_vertex_array`, `GL_EXT_vertex_array`,
  `GL_EXT_paletted_texture`, `GL_EXT_blend_color`,
  `GL_EXT_blend_minmax`, `GL_EXT_blend_subtract`, `GL_EXT_abgr`,
  `GL_EXT_bgra`, `GL_EXT_texture_lod_bias`,
  `GL_EXT_texture_filter_anisotropic`
- `GL_SGIS_generate_mipmap`, `GL_SGIS_texture_edge_clamp`,
  `GL_APPLE_transform_hint`

`glGetString(GL_EXTENSIONS)` tells which ones the renderer has. Each
extension also has its `PFN...PROC` type. To find a function by its
name at run time, as `wglGetProcAddress` does on Windows, use the Code
Fragment Manager:

```c
ConnectionID conn;
Ptr entry, addr;
SymClass cls;
Str255 err;
if (GetSharedLibrary("\pOpenGLLibrary", kPowerPCArch, kReferenceCFrag,
                     &conn, &entry, err) == noErr &&
    FindSymbol(conn, "\pglActiveTextureARB", &addr, &cls) == noErr) {
  PFNGLACTIVETEXTUREARBPROC f = (PFNGLACTIVETEXTUREARBPROC)addr;
}
```

## How the files are made

- `tools/gen-gl.py gl.xml` writes `include/GL/gl.h` and
  `include/GL/glext.h` from the Khronos OpenGL registry
  ([gl.xml](https://github.com/KhronosGroup/OpenGL-Registry/blob/main/xml/gl.xml),
  Apache-2.0). It also writes `data/opengl-exports.json`: the functions
  of `OpenGLLibrary` and `OpenGLUtility`, read from the headers.
- `include/GL/glu.h` and `include/AGL/agl.h` are written by hand, from
  the GLU 1.3 specification and the AGL documentation.
- `tools/link/gen-imports.py` adds `data/opengl-exports.json` to
  `tools/link/imports.json`.

## Tests

`tests/gl.c` makes an off-screen context, clears it, reads the pixels
back, calls GLU and finds a function with `FindSymbol`. `tests/run.sh` runs it in `tools/simppc.py`. The
simulator's fake OpenGL (`tools/fakegl.py`) is not a renderer. It keeps
the AGL objects, the current context, the viewport and the off-screen
buffer, and writes the clear color on `glClear`. The other functions do
nothing. The simulator also fakes `GetSharedLibrary` and `FindSymbol`
for the libraries that it fakes. It checks the imports and the calls, not the drawing.
