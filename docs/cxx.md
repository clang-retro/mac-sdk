# C++

The SDK compiles and links C++ programs for 68k and PowerPC Macs. The
programs can use all of the language: exceptions, run-time type
information (RTTI), and static constructors and destructors. They can
also use GCC's C++ library, libstdc++ 15.2 (strings, containers,
algorithms, smart pointers, `std::function`, iostreams and string
streams).

## Build the libraries

Build the runtime library and the C library first (see
[Building applications](building.md)). Then build the C++ library:

```sh
CLANG=path/to/clang LLVM_SRC=path/to/clang-retro/llvm \
  GCC_SRC=path/to/gcc-15.2.0 runtime/build-libstdcxx.sh
```

- `LLVM_SRC` is a checkout of clang-retro/llvm. The script reads only
  `libunwind`.
- `GCC_SRC` is an unpacked GCC 15.2.0 release. The script does not
  change it.
- `CPUS` selects the CPUs. The default is `m68k ppc`.

The script needs `make` and a POSIX shell for GCC's `configure`. It
takes about 15 minutes for each CPU on 4 processors.

The script writes these files:

| File | Contents |
| --- | --- |
| `lib/include/c++/15.2.0/` | the headers of libstdc++ |
| `lib/include/c++/15.2.0/m68k-apple-macos/` | the headers for 68k (`bits/c++config.h` and others) |
| `lib/include/c++/15.2.0/powerpc-apple-macos9/` | the headers for PowerPC |
| `lib/m68k/` and `lib/ppc/` | `libstdc++.a`, `libsupc++.a` and `libunwind.a` |
| `lib/share/licenses/libstdc++/` | `COPYING3` and `COPYING.RUNTIME` |

## Build a program

Give the C++ sources (`.cpp`, `.cc` or `.cxx`) to `tools/macapp.py`:

```sh
python3 tools/macapp.py -o Hello.bin --console window hello.cpp
```

`macapp.py` does these steps:

1. It compiles the C++ sources with the headers of libstdc++ before the
   headers of the C library.
2. It compiles all sources with `-ffunction-sections` and
   `-fdata-sections`.
3. It links `libstdc++.a` and `libunwind.a` before the C library.

C and C++ sources can be in the same program.

## How exceptions work

The two CPUs use different unwind information. On both CPUs,
`libunwind.a` (LLVM's unwinder) reads the information, and libstdc++'s
personality routine (`__gxx_personality_v0`) finds the handler.

### 68k

clang writes DWARF call frame information (CFI) in `.eh_frame` for each
function. `tools/link/codelink.py` puts all of `.eh_frame` in one block
in the read-only data, between the symbols `__eh_frame_start` and
`__eh_frame_end`. libunwind searches this block for the frame
description entry (FDE) of each return address. The startup code
(`start.S`) relocates the absolute pointers in the block (the
personality routine and the language-specific data areas) with all the
other pointers.

The 68k CFI of LLVM had three errors, which clang-retro/llvm corrects:

- The return address column was the register A0. Now it is the program
  counter, DWARF register 24, as in GCC.
- The CFA offsets after the stack adjustment and after `LINK` were
  negative. Now they are positive.
- The CFI did not tell where `MOVEM` saved the callee-saved registers.
  Now it does.

clang-retro/llvm also has a 68k port of libunwind
(`Registers_m68k`). It keeps D0 to D7, A0 to A7 and the program counter.
The landing pad gets the exception object in D0 and the type selector
in D1.

On 68k, a function returns a pointer in A0 and an integer in D0. Thus
`_Unwind_GetLanguageSpecificData` of libunwind returns `void *` on 68k,
as in the `unwind.h` that libstdc++ uses.

### PowerPC

clang writes a traceback table after the code of each function, as on
AIX. The table gives the saved registers, the frame size and a pointer
to the exception information (the language-specific data area and the
personality routine). libunwind finds the table: it reads forward from
the return address to the next zero word. clang-retro/llvm lets
libunwind do this on classic Mac OS too, not only on AIX.

clang's assembler cannot assemble libunwind's assembly files for XCOFF.
`runtime/unwind/xcoff-ppc.c` has the two assembly functions of
libunwind (`__unw_getcontext` and `Registers_ppc::jumpto`) as naked
functions.

## What the linkers do for C++

`codelink.py` (68k) and `peflink.py` (PowerPC) do these steps:

- They keep one copy of each inline function, template instance and
  type information object. On 68k, they keep the first COMDAT group of
  each name. On PowerPC, these objects are weak, and the linker uses the
  first definition.
- A strong definition replaces a weak one.
- They remove the sections (68k) or csects (PowerPC) that nothing
  reaches from the entry point and the constructor and destructor
  tables. On 68k, they also remove the FDEs of the removed functions.
- `peflink.py` calculates a word that is the difference of two symbols
  (an `R_POS` and an `R_NEG` relocation at the same address). The
  traceback tables and the exception tables use these words.

`--no-gc-sections` tells a linker to keep all sections.

## Static constructors and destructors

On 68k, clang puts the constructors in `.init_array`. On PowerPC, clang
makes a `__sinit` function for each source file, as on AIX. The C
library's startup code (`__crt_main`) calls them before `main`. The
destructors of global objects run at `exit`, through `__cxa_atexit`.

## Size

libstdc++ is large next to the memory of a classic Mac. These are the
code sizes of two small programs:

| Program | 68k | PowerPC |
| --- | --- | --- |
| `throw` and `catch` of `std::runtime_error`, `printf` | 53 KB | 52 KB |
| `std::cout << "Hello" << std::endl` | 376 KB | 394 KB |

iostreams bring in the locale support of libstdc++. If the program must
be small, use `<cstdio>` instead of `<iostream>`. Give a program that
uses iostreams a partition of at least 4 MB (`macapp.py --size 4096`).

On 68k, programs with more than 32 KB of code use 32-bit addresses
(`-mcmodel=large`). `macapp.py` selects this automatically.

## Limits

- libstdc++ uses the thread model `posix`, over the C library's
  cooperative threads (see docs/threads.md). `std::thread`,
  `std::mutex`, `std::condition_variable`, `std::call_once`,
  `std::async` and `std::this_thread::sleep_for` work. A thread runs
  until it waits, locks a locked mutex, sleeps or calls
  `std::this_thread::yield`. `thread_local` variables use emulated
  TLS (see docs/threads.md).
- `std::filesystem` is not available. The library is built without it
  (`--disable-libstdcxx-filesystem-ts`). Use the C library's directory
  functions (`opendir`, `readdir`, `stat`, `mkdir`) instead.
- On 68k, the unwinder does not restore the floating-point registers.
  Thus do not compile C++ code that uses exceptions with `-m68881` or
  for the 68040's FPU. The SDK uses software floating point by default.
- The library is static. There is no shared library for C++.
- libstdc++ has only the new C++11 ABI of `std::string` and
  `std::list` (`--disable-libstdcxx-dual-abi`).
- The library does not print a message when it calls
  `std::terminate` (`--disable-libstdcxx-verbose`). This keeps the
  demangler out of the programs.

## How the library is built

`runtime/build-libstdcxx.sh` does these steps for each CPU:

1. It compiles libunwind from `LLVM_SRC`.
2. It copies the parts of GCC that it uses to its build directory.
3. It applies `runtime/libstdcxx/gcc-15.2.0.patch` to the copy. The top
   of the patch file gives each change.
4. It runs the `configure` script of `libstdc++-v3` with clang as the
   compiler. GCC does not compile anything. Thus the library has the
   same ABI as the programs that clang compiles.
5. It installs the headers and the libraries in `lib/`.

GCC has no target for classic Mac OS. The script gives `configure` the
host `m68k-unknown-elf` or `powerpc-unknown-elf` with `--with-newlib`.
These are the settings for a C library without an operating system
layer. picolibc comes from newlib.

The compilers for the classic Mac OS define the macro `Macintosh`. The
patch removes this macro in `<text_encoding>`, where `Macintosh` is the
name of an encoding. The patch also removes the lock of the 68k atomic
operations (`config/cpu/m68k/atomicity.h`): the threads are cooperative,
so a plain read, change and write is atomic between them, and clang's
assembler does not accept GCC's `jbne`.

## Tests

`tests/cxx.cpp` tests the C++ runtime and library, and
`tests/cxxthread.cpp` tests its threads. With `GCC_SRC` set,
`tests/run.sh` builds the C++ library and runs the tests in
`tools/sim68k.py` (as a 68000 and a 68020) and in `tools/simppc.py`.

The test also passes on real Systems: the 68k build on System 7.6.1
(QEMU `q800`), and the PowerPC build and the fat build on Mac OS 9.2.1
(QEMU `mac99`).

## Licence

libstdc++ is under the GPL 3 with the GCC Runtime Library Exception
(`lib/share/licenses/libstdc++`). A program that clang compiles and
links with libstdc++ can have any licence: clang is not a work based on
GCC, so the compilation is an "Eligible Compilation Process". If you
distribute the library itself, also make available its source: the GCC
15.2.0 release and `runtime/libstdcxx/` of this repository.

libunwind is under the Apache License 2.0 with LLVM Exceptions.
