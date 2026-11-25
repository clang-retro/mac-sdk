# QuickDraw 3D (Quesa)

PowerPC programs can use QuickDraw 3D through Quesa. Quesa is an
open-source implementation of the QuickDraw 3D API. It draws with
OpenGL. The SDK has a script that builds Quesa as a static library for
the classic Mac OS on PowerPC.

## Summary

| Item | Value |
| --- | --- |
| Source | <https://github.com/jwwalker/Quesa>, commit `883c9a0d` (July 2007) |
| License | BSD 3-clause (Quesa's own license) |
| Build script | `extras/quesa/build.sh QUESA_SRC LIB` |
| Library | `LIB/ppc/libquesa.a` |
| Headers | `LIB/include/Quesa/`: `Quesa*.h`, and `QD3D*.h` names for them |
| Renderers | The interactive renderer (OpenGL), the wireframe renderer, and the others of Quesa 1.8 |
| Draw contexts | Pixmap (AGL off-screen) and Macintosh window (AGL drawable) |
| CPU | PowerPC only. There is no OpenGL for 68k Macs. |

## Why this commit

Quesa stopped building for the classic Mac OS in the commit after
`883c9a0d` ("Remove Classic", `af2f641c`). Later versions also need
GLSL shaders, which Apple OpenGL for Mac OS 9 does not have.
`build.sh` stops when the checkout is at a different commit.

## Requirements

- The C library and the C++ library in `LIB`: run `runtime/build.sh`,
  `runtime/build-libc.sh` and `runtime/build-libstdcxx.sh` first.
  Quesa is C++ code.
- clang-retro's clang. `CLANG` names it (default: `clang`). `llvm-ar`
  must be in the same directory.
- A Quesa checkout at commit `883c9a0d218bd18c17f2f62024c0e57f2d5d4a07`.
  Only `Development/Source` and `SDK/Includes` are necessary.

## Build Quesa

```sh
git clone https://github.com/jwwalker/Quesa.git quesa
git -C quesa checkout 883c9a0d218bd18c17f2f62024c0e57f2d5d4a07
CLANG=/path/to/clang extras/quesa/build.sh quesa /path/to/lib
```

The script does these steps:

1. It copies the sources to a temporary directory. It does not change
   the checkout.
2. It applies `extras/quesa/quesa.patch` (see below).
3. It compiles the 139 files of `extras/quesa/files.txt` as C++98. This
   is the file list of the "Classic ShLib Release" target of Quesa's
   CodeWarrior project. CodeWarrior also compiled these `.c` files as
   C++.
4. It writes `LIB/ppc/libquesa.a` and copies the headers to
   `LIB/include/Quesa/`.

## Build a program

Compile the program as C++ and link `libquesa.a`:

```sh
CLANG=/path/to/clang python3 tools/macapp.py --cpu ppc --lib /path/to/lib \
  -o Teapot.bin -- -Wno-pragma-pack -I/path/to/lib/include/Quesa \
  teapot.cpp /path/to/lib/ppc/libquesa.a
```

- Include `QD3D.h`, `QD3DGeometry.h` and the other QuickDraw 3D names,
  or the `Quesa*.h` names. Each `QD3D*.h` file includes the `Quesa*.h`
  file of the same name.
- At least one source file must be C++. `macapp.py` links the C++
  library only for a program that has C++ sources.
- `Quesa.h` sets `#pragma options align=power` and does not reset it.
  This is the QuickDraw 3D alignment, and it is the PowerPC default.
  `-Wno-pragma-pack` stops the warning about it.
- `Quesa.h` finds the classic Mac OS from clang's `macintosh` macro. Do
  not define `QUESA_OS_MACINTOSH` yourself.

## Changes to Quesa

`quesa.patch` changes these items:

| File | Change |
| --- | --- |
| `SDK/Includes/Quesa/Quesa.h` | Also selects the Mac OS when `macintosh` is defined and `__MACH__` is not. |
| `E3Viewer.c`, `E3MacDrawContext.c`, `E3MacViewer.h` and its copies | `<QuickDraw.h>` becomes `<Quickdraw.h>`, the name of the SDK's header. |
| `Renderers/Common/GLUtils.c` | `GLGetProcAddress` finds OpenGL functions with `GetSharedLibrary` and `FindSymbol`. In Quesa's classic build, it always gave NULL. |

The files in `extras/quesa/compat/` replace what CodeWarrior and
Apple's Universal Interfaces gave:

| File | Gives |
| --- | --- |
| `quesa-classic.h` | The settings of Quesa's prefix file (`QUESA_OS_MACINTOSH`, `Q3_DEBUG 0`, `Q3_EXPORT_SYMBOLS 1`), `<limits.h>`, `<CarbonAccessors.h>`, and `kGeneralFailureErr`. The script includes it before each source file. |
| `CFBundle.h`, `CFURL.h`, `CFString.h` | The Core Foundation type names that Quesa's prototypes use. The classic build does not call Core Foundation. |

## Things that the library does not do

- Plug-ins: Quesa looks for plug-ins in the Extensions folder and next
  to the application. It loads each one with `GetDiskFragment`. The
  SDK cannot link a plug-in (a CFM shared library), so only the
  renderers that are in `libquesa.a` are available.
- Programs that are already linked against Apple's "QuickDraw™ 3D Lib":
  the library is static, so these programs do not use it. A shared
  library build of Quesa needs a shared library linker.
- Debug builds: `Q3_DEBUG` is 0. Quesa's debug build needs
  `E3MacCFMStackCrawl.c`, which is CodeWarrior assembler.

## Test

`tests/quesa.cpp` starts Quesa, makes a box, gets its bounds through a
view, and renders it with the interactive renderer into a 16 x 8
pixmap. Then it checks that the pixmap has the clear colour. `tests/run.sh`
builds Quesa and runs this test in `tools/simppc.py` when `QUESA_SRC`
names the checkout (with `LLVM_SRC`, `PICOLIBC_SRC` and `GCC_SRC`). The
fake OpenGL of `tools/fakegl.py` does the clear. The GitHub workflow
gets Quesa and runs the test.
