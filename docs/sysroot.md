# The classic Mac OS sysroot

The sysroot is one directory that has all the parts that are necessary
to compile and link classic Mac OS applications. It is for 68k and
PowerPC, from System 6 to Mac OS 9. You do not need this source tree to
use it. You do not need to build the libraries.

## Get the sysroot

The nightly release of clang-retro/dos-sdk has the sysroot as
`clang-dos-sysroot-macos-classic.tar.xz`. To get it, use
`get-toolchain.sh` from dos-sdk:

```sh
packaging/get-toolchain.sh -s macos-classic ~/retro    # gives ~/retro/macos-classic
```

You also need clang from clang-retro/llvm, for example the clang of the
same release (`packaging/get-toolchain.sh ~/retro/toolchain`), and
Python 3.

## Build an application

`tools/macapp.py` in the sysroot compiles and links. It finds the
headers and the libraries in the sysroot:

```sh
S=~/retro/macos-classic
CLANG=path/to/clang python3 $S/tools/macapp.py -o Hello.bin $S/examples/hello.c
CLANG=path/to/clang python3 $S/tools/macapp.py --cpu 68k -o Hello68.bin hello.c
CLANG=path/to/clang python3 $S/tools/macapp.py --cpu ppc -o HelloPPC.bin hello.cpp
```

The default output is a fat application (68k and PowerPC) in MacBinary
II. To put it on a Mac disk, use hfsutils (`hcopy -m`). The options of
`macapp.py` are in [Building applications](building.md).

To compile without `macapp.py`, give clang the two include directories:

```sh
clang --target=m68k-apple-macos -isystem $S/lib/include -I$S/include -c a.c
clang --target=powerpc-apple-macos9 -isystem $S/lib/include -I$S/include -c a.c
```

Then link the objects with `$S/tools/macapp.py --cpu 68k` or
`--cpu ppc`.

## Contents

| Path | Contents |
| --- | --- |
| `include/` | The Toolbox headers (`Multiverse.h`, `Quickdraw.h`, ...), with MacTCP, Open Transport, the Shared Library Manager, QuickTime, OpenGL and Carbon accessors |
| `lib/include/` | The headers of picolibc (the C library) and of libstdc++ (`c++/`) |
| `lib/m68k/`, `lib/ppc/` | `libmacrt.a` (the compiler-rt builtins and soft float), `libc.a` (picolibc), `libmacos.a` (the Mac layer: files, sockets, threads, Open Transport, the Shared Library Manager client), `libmacconsole.a` (the console window), `libstdc++.a`, `libsupc++.a`, `libunwind.a` |
| `tools/macapp.py` | The compiler and linker driver |
| `tools/link/` | The linkers (`codelink.py` for 68k, `peflink.py` for PowerPC, `fatlink.py`), the start code, and the index of the shared library imports |
| `tools/sim68k.py`, `tools/simppc.py` | CPU emulators that run small programs (they need the `unicorn` Python package) |
| `examples/` | The example programs |
| `share/licenses/` | The license of each part |

## Build the sysroot

`build-sysroot.sh` in mac-sdk builds the sysroot. It takes about two
minutes.

```sh
CLANG=path/to/clang LLVM_SRC=path/to/llvm PICOLIBC_SRC=path/to/picolibc \
  GCC_SRC=path/to/gcc-15.2.0 sh build-sysroot.sh out/macos-classic
CLANG=path/to/clang sh tests/sysroot.sh out/macos-classic
```

`GCC_SRC` is optional. If you do not set it, the sysroot has no C++
library. It needs meson, ninja and make. `tests/sysroot.sh` builds
programs with the sysroot only, and runs them in the CPU emulators.

## Licenses

All the files in the sysroot are free software. No file from Apple is
in the sysroot.

- The SDK (headers, Mac layer, linkers, tools): AGPL-3.0-only. The
  headers made from Multiversal Interfaces are also MIT.
- picolibc: BSD-style licenses.
- compiler-rt builtins and libunwind: Apache-2.0 with the LLVM
  exception.
- libstdc++ and libsupc++: GPL-3.0 with the GCC Runtime Library
  Exception.
