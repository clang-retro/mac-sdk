"""A fake Apple OpenGL for tools/simppc.py: the agl* and gl* functions of
OpenGLLibrary and the glu* functions of OpenGLUtility.

It is not a renderer. It keeps what a program can read back: the pixel
formats and contexts of AGL, the current context, the off-screen buffer
of aglSetOffScreen, the clear color, the viewport and the enabled
capabilities, and it writes the clear color into the off-screen buffer
on glClear, as 32-bit ARGB pixels. glReadPixels reads that buffer back.
glGetString names this fake. The other functions do nothing and give 0.
This checks the import glue, the calls and their arguments, not the
drawing.

Copyright (C) clang-retro contributors.
SPDX-License-Identifier: AGPL-3.0-only
"""
import os
import re
import struct

# The AGL attributes that are followed by a value.
AGL_VALUED = {2, 3, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 50, 55, 56, 70}
AGL_RGBA, AGL_DOUBLEBUFFER, AGL_PIXEL_SIZE = 4, 5, 50

GL_COLOR_BUFFER_BIT = 0x4000
GL_VENDOR, GL_RENDERER, GL_VERSION, GL_EXTENSIONS = 0x1F00, 0x1F01, 0x1F02, 0x1F03
GL_VIEWPORT, GL_MAX_TEXTURE_SIZE = 0x0BA2, 0x0D33
GL_RGBA, GL_BGRA, GL_UNSIGNED_BYTE = 0x1908, 0x80E1, 0x1401
GL_INVALID_ENUM = 0x0500
GLU_VERSION, GLU_EXTENSIONS = 100800, 100801

STRINGS = {GL_VENDOR: "clang-retro", GL_RENDERER: "simppc fake OpenGL",
           GL_VERSION: "1.2 fake",
           GL_EXTENSIONS: "GL_ARB_multitexture GL_EXT_abgr GL_EXT_bgra "
                          "GL_EXT_compiled_vertex_array"}
# The headers that declare the functions of OpenGLLibrary and
# OpenGLUtility, from the directory of this file.
HEADERS = ("GL/gl.h", "GL/glext.h", "GL/glu.h", "AGL/agl.h")
INCLUDE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                       "include")


def library_names():
    """Give the names of the agl*, gl* and glu* functions that the SDK's
    OpenGL headers declare: the functions that the real libraries
    export."""
    names = set()
    for header in HEADERS:
        with open(os.path.join(INCLUDE, header), encoding="latin-1") as f:
            names.update(re.findall(r"\b((?:agl|glu?)[A-Z]\w*)\s*\(",
                                    f.read()))
    return names


GLU_STRINGS = {GLU_VERSION: "1.3", GLU_EXTENSIONS: ""}
GLU_ERRORS = {0: "no error", 0x0500: "invalid enumerant",
              0x0501: "invalid value", 0x0502: "invalid operation",
              0x0503: "stack overflow", 0x0504: "stack underflow",
              0x0505: "out of memory", 100900: "invalid enumerant",
              100901: "invalid value", 100902: "out of memory",
              100903: "incompatible gl version",
              100904: "invalid operation"}


class FakeGL:
    """The state of the fake OpenGL, and its functions by name."""

    def __init__(self, uc, arg, sarg, farg, ret, alloc):
        """Keep the simulator's CPU and its argument helpers: arg(n) and
        sarg(n) read integer argument n, farg(n) floating-point argument
        n, ret sets the result and alloc(size) gives memory."""
        self.uc, self.arg, self.sarg, self.farg = uc, arg, sarg, farg
        self.ret, self.alloc = ret, alloc
        self.next_id = 0x00A10000           # handles: not addresses
        self.formats = {}                   # id -> {attribute: value}
        self.contexts = {}                  # id -> dict
        self.current = 0
        self.error = 0
        self.strings = {}                   # text -> address
        self.calls = 0
        self.swaps = 0
        self.names = library_names()
        self.funcs = {
            "aglChoosePixelFormat": self.choose_pixel_format,
            "aglDestroyPixelFormat": lambda: self.formats.pop(arg(0), None),
            "aglNextPixelFormat": lambda: ret(0),
            "aglDescribePixelFormat": self.describe_pixel_format,
            "aglDevicesOfPixelFormat": self.devices_of_pixel_format,
            "aglQueryRendererInfo": lambda: ret(self.new_id()),
            "aglNextRendererInfo": lambda: ret(0),
            "aglDescribeRenderer": self.describe_renderer,
            "aglCreateContext": self.create_context,
            "aglDestroyContext": self.destroy_context,
            "aglSetCurrentContext": self.set_current_context,
            "aglGetCurrentContext": lambda: ret(self.current),
            "aglSetDrawable": self.set_drawable,
            "aglGetDrawable": lambda: ret(self.context(arg(0)).get("port", 0)
                                          if self.context(arg(0)) else 0),
            "aglSetOffScreen": self.set_off_screen,
            "aglSwapBuffers": self.swap_buffers,
            "aglGetVersion": self.get_version,
            "aglGetError": lambda: ret(0),
            "aglErrorString": lambda: ret(self.string("no error")),
            "aglGetVirtualScreen": lambda: ret(0),
            "aglGetInteger": self.agl_get_integer,
            "glClearColor": self.clear_color,
            "glClear": self.clear,
            "glViewport": self.viewport,
            "glGetIntegerv": self.get_integerv,
            "glGetString": self.get_string,
            "glGetError": self.get_error,
            "glReadPixels": self.read_pixels,
            "glEnable": lambda: self.state().setdefault("on", set()).add(arg(0)),
            "glDisable": lambda: self.state().setdefault("on", set()).discard(arg(0)),
            "glIsEnabled": lambda: ret(arg(0) in self.state().get("on", set())),
            "gluErrorString": lambda: ret(self.string(
                GLU_ERRORS.get(arg(0), "unknown error"))),
            "gluGetString": lambda: ret(self.string(GLU_STRINGS[arg(0)])
                                        if arg(0) in GLU_STRINGS else 0),
            "gluNewQuadric": lambda: ret(self.new_id()),
            "gluNewTess": lambda: ret(self.new_id()),
            "gluNewNurbsRenderer": lambda: ret(self.new_id()),
            "gluCheckExtension": self.check_extension,
        }

    # ------------------------------------------------------------ helpers

    def new_id(self):
        """Give a new handle."""
        self.next_id += 16
        return self.next_id

    def string(self, text):
        """Give the address of a C string with text, made once."""
        if text not in self.strings:
            data = text.encode("mac_roman") + b"\0"
            addr = self.alloc(len(data))
            self.uc.mem_write(addr, data)
            self.strings[text] = addr
        return self.strings[text]

    def cstring(self, addr):
        """Read the C string at addr."""
        out = bytearray()
        while addr and len(out) < 4096:
            c = self.uc.mem_read(addr + len(out), 1)[0]
            if not c:
                break
            out.append(c)
        return out.decode("mac_roman")

    def context(self, ctx):
        """Give the context with handle ctx, or None."""
        return self.contexts.get(ctx)

    def state(self):
        """Give the current context, or a dict that nothing keeps."""
        return self.contexts.get(self.current, {})

    def call(self, name):
        """Run the function name of the fake: a known one, or nothing."""
        self.calls += 1
        f = self.funcs.get(name)
        if f is None:
            self.ret(0)
        else:
            f()

    def exports(self, name):
        """Tell if the real OpenGLLibrary or OpenGLUtility has the
        function name: one that the fake runs, or one that the SDK's
        headers declare. FindSymbol finds no other name."""
        return name in self.funcs or name in self.names

    def summary(self):
        """Print what the program did with OpenGL."""
        if self.calls:
            print(f"fakegl: {self.calls} calls, {len(self.contexts)} "
                  f"contexts left, {self.swaps} buffer swaps")

    # ---------------------------------------------------------------- AGL

    def choose_pixel_format(self):
        """aglChoosePixelFormat(gdevs, ndev, attribs): a format with the
        attributes of the list. Level, stereo and color index have none."""
        attrs, p = {}, self.arg(2)
        while p:
            a = struct.unpack(">i", bytes(self.uc.mem_read(p, 4)))[0]
            p += 4
            if a == 0:
                break
            v = 1
            if a in AGL_VALUED:
                v = struct.unpack(">i", bytes(self.uc.mem_read(p, 4)))[0]
                p += 4
            attrs[a] = v
        if attrs.get(3, 0) or 6 in attrs or AGL_RGBA not in attrs:
            self.ret(0)
            return
        pix = self.new_id()
        self.formats[pix] = attrs
        self.ret(pix)

    def describe_pixel_format(self):
        """aglDescribePixelFormat(pix, attrib, &value)."""
        attrs = self.formats.get(self.arg(0))
        if attrs is None:
            self.ret(0)
            return
        a = self.arg(1)
        values = {AGL_RGBA: 1, AGL_PIXEL_SIZE: 32, 8: 8, 9: 8, 10: 8, 11: 8,
                  AGL_DOUBLEBUFFER: int(AGL_DOUBLEBUFFER in attrs),
                  12: attrs.get(12, 0), 13: attrs.get(13, 0)}
        self.uc.mem_write(self.arg(2), struct.pack(">i", values.get(a, 0)))
        self.ret(1)

    def devices_of_pixel_format(self):
        """aglDevicesOfPixelFormat(pix, &ndevs): no devices here."""
        if self.arg(1):
            self.uc.mem_write(self.arg(1), struct.pack(">i", 0))
        self.ret(0)

    def describe_renderer(self):
        """aglDescribeRenderer(rend, prop, &value): 0 for each property."""
        self.uc.mem_write(self.arg(2), struct.pack(">i", 0))
        self.ret(1)

    def create_context(self):
        """aglCreateContext(pix, share)."""
        if self.arg(0) not in self.formats:
            self.ret(0)
            return
        ctx = self.new_id()
        self.contexts[ctx] = {"clear": (0.0, 0.0, 0.0, 0.0),
                              "viewport": (0, 0, 0, 0)}
        self.ret(ctx)

    def destroy_context(self):
        """aglDestroyContext(ctx)."""
        ctx = self.arg(0)
        if self.current == ctx:
            self.current = 0
        self.ret(int(self.contexts.pop(ctx, None) is not None))

    def set_current_context(self):
        """aglSetCurrentContext(ctx): NULL makes no context current."""
        ctx = self.arg(0)
        if ctx and ctx not in self.contexts:
            self.ret(0)
            return
        self.current = ctx
        self.ret(1)

    def set_drawable(self):
        """aglSetDrawable(ctx, port): the viewport becomes the port's size
        the first time, as on a Mac; the fake has no port size, so 0."""
        c = self.context(self.arg(0))
        if c is None:
            self.ret(0)
            return
        c["port"] = self.arg(1)
        c.pop("buffer", None)
        self.ret(1)

    def set_off_screen(self):
        """aglSetOffScreen(ctx, width, height, rowbytes, baseaddr)."""
        c = self.context(self.arg(0))
        w, h, rb, base = self.sarg(1), self.sarg(2), self.sarg(3), self.arg(4)
        if c is None or w <= 0 or h <= 0 or rb < 4 * w or not base:
            self.ret(0)
            return
        c["buffer"] = (base, w, h, rb)
        c.pop("port", None)
        if c["viewport"] == (0, 0, 0, 0):
            c["viewport"] = (0, 0, w, h)
        self.ret(1)

    def swap_buffers(self):
        """aglSwapBuffers(ctx)."""
        self.swaps += 1

    def get_version(self):
        """aglGetVersion(&major, &minor): AGL 2.0."""
        for i, v in ((0, 2), (1, 0)):
            if self.arg(i):
                self.uc.mem_write(self.arg(i), struct.pack(">i", v))

    def agl_get_integer(self):
        """aglGetInteger(ctx, pname, params): 0 for each option."""
        if self.arg(2):
            self.uc.mem_write(self.arg(2), struct.pack(">i", 0))
        self.ret(1)

    # ----------------------------------------------------------------- GL

    def clear_color(self):
        """glClearColor(r, g, b, a)."""
        self.state()["clear"] = tuple(
            min(1.0, max(0.0, self.farg(i))) for i in range(4))

    def clear(self):
        """glClear(mask): the color buffer of an off-screen context gets
        the clear color."""
        c = self.state()
        if not (self.arg(0) & GL_COLOR_BUFFER_BIT) or "buffer" not in c:
            return
        base, w, h, rb = c["buffer"]
        r, g, b, a = (int(round(x * 255)) for x in c["clear"])
        row = bytes([a, r, g, b]) * w
        for y in range(h):
            self.uc.mem_write(base + y * rb, row)

    def viewport(self):
        """glViewport(x, y, width, height)."""
        self.state()["viewport"] = tuple(self.sarg(i) for i in range(4))

    def get_integerv(self):
        """glGetIntegerv(pname, params): the viewport and the largest
        texture; 0 for the others."""
        pname, p = self.arg(0), self.arg(1)
        if pname == GL_VIEWPORT:
            v = self.state().get("viewport", (0, 0, 0, 0))
            self.uc.mem_write(p, struct.pack(">4i", *v))
        elif pname == GL_MAX_TEXTURE_SIZE:
            self.uc.mem_write(p, struct.pack(">i", 256))
        else:
            self.uc.mem_write(p, struct.pack(">i", 0))

    def get_string(self):
        """glGetString(name): NULL without a current context."""
        name = self.arg(0)
        if not self.current or name not in STRINGS:
            if name not in STRINGS:
                self.error = self.error or GL_INVALID_ENUM
            self.ret(0)
            return
        self.ret(self.string(STRINGS[name]))

    def get_error(self):
        """glGetError: the first error since the last call."""
        self.ret(self.error)
        self.error = 0

    def read_pixels(self):
        """glReadPixels(x, y, width, height, format, type, data), from the
        off-screen buffer, with row 0 at the bottom as in OpenGL."""
        c = self.state()
        x, y, w, h = (self.sarg(i) for i in range(4))
        fmt, typ, data = self.arg(4), self.arg(5), self.arg(6)
        if "buffer" not in c or typ != GL_UNSIGNED_BYTE or \
                fmt not in (GL_RGBA, GL_BGRA):
            self.error = self.error or GL_INVALID_ENUM
            return
        base, bw, bh, rb = c["buffer"]
        out = bytearray()
        for row in range(y, y + h):
            src = base + (bh - 1 - row) * rb
            for col in range(x, x + w):
                if 0 <= row < bh and 0 <= col < bw:
                    a, r, g, b = self.uc.mem_read(src + 4 * col, 4)
                else:
                    a = r = g = b = 0
                out += bytes([r, g, b, a] if fmt == GL_RGBA else [b, g, r, a])
        self.uc.mem_write(data, bytes(out))

    # ---------------------------------------------------------------- GLU

    def check_extension(self):
        """gluCheckExtension(name, list): whether name is a word of list."""
        name, words = self.cstring(self.arg(0)), self.cstring(self.arg(1))
        self.ret(int(name in words.split()))
