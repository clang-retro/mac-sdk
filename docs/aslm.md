# Shared Library Manager (ASLM)

The Apple Shared Library Manager (ASLM) loads shared libraries on 68k
Macintosh computers. Open Transport's 68k libraries are ASLM
libraries. The SDK has a small C interface to the client side of ASLM,
in `LibraryManager.h`. The C library (`runtime/libc/aslm.c`) has the
calls.

There is no ASLM on PowerPC. On PowerPC, shared libraries are Code
Fragment Manager fragments, and the SDK links to them directly.

## What the SDK has

| Name | Kind | What it does |
| --- | --- | --- |
| `InitLibraryManager` | Apple call | Starts the program's use of ASLM. |
| `CleanupLibraryManager` | Apple call | Ends the program's use of ASLM. The C library calls it when the program ends. |
| `ASLMPresent` | SDK call | Gives `true` when ASLM is installed. Loads ASLM when it is installed but not loaded. |
| `ASLMLoadFunctionSet` | SDK call | Loads the library that has a function set. |
| `ASLMCallC` | SDK call | Calls a function of the MPW C calling convention. |
| `ASLMCallPascal` | SDK call | Calls a Pascal function. |
| `ASLM_FUNCTION_SET` | SDK macro | Makes the record that names a function set. |
| `ASLM_FUNCTION` | SDK macro | Makes a stub for one function of a function set. |

Apple's own client header for ASLM is C++, and Apple's LibraryBuilder
tool made the stubs from an export file. The SDK does not have these.
The macros do the work of LibraryBuilder for a C program.

## Requirements

- A 68k Macintosh with ASLM installed. Open Transport on 68k installs
  ASLM. Other software also installs it as the `Shared Library
  Manager` extension.
- Without ASLM, `InitLibraryManager` gives `kASLMNotPresentErr`
  (-3132), and each stub gives the same value.

## How to call a function of a library

1. Make the record of the function set. Give the identifier of the set,
   the version that you want and the oldest version that you accept.
2. Make a stub for each function. The index is the number of the
   function in the export file of the library.
3. Call `InitLibraryManager`, then `ASLMLoadFunctionSet`.
4. Call each stub through `ASLMCallC` or `ASLMCallPascal`. The first
   call binds the stub through ASLM. The next calls go directly to the
   function.

Example (a function of Open Transport's 68k utilities library):

```c
#include <LibraryManager.h>

ASLM_FUNCTION_SET(utils, "OTClnt$LclUtils", 0x0100, 0x0100);
ASLM_FUNCTION(my_free_mem, utils, 31); /* OTFreeMem */
extern const char my_free_mem[];

void free_block(void *p) {
  long args[1];
  args[0] = (long)p;
  ASLMCallC(my_free_mem, 1, args);
}

int start(void) {
  if (InitLibraryManager(kASLMDefaultPoolSize, kASLMDefaultZone,
                         kASLMDefaultMemory) != noErr)
    return 0;
  return ASLMLoadFunctionSet("OTClnt$LclUtils") == noErr;
}
```

## Calling conventions

Do not call a stub directly from C. Use `ASLMCallC` or
`ASLMCallPascal`, for these reasons:

- MPW code can change register D2. Clang keeps a value in D2 across a
  call.
- An MPW C function gives a pointer result in D0. Clang reads a pointer
  result from A0.

`ASLMCallC` pushes the arguments from right to left, as MPW C does,
and gives the value of D0. Each argument is 4 bytes.

`ASLMCallPascal` reserves space for the result, then pushes the
arguments from left to right. Set bit `i` of `wordMask` when argument
`i` is 2 bytes (a `short` or a `Boolean`). Give `resultSize` as 0, 2 or
4. A `Boolean` result is in bits 8 to 15 of the result. A `Boolean`
argument is 2 bytes, with the value in bits 8 to 15 (pass `0x100` for
`true`).

## Open Transport on 68k

The C library uses this interface for Open Transport on 68k. When
ASLM and Open Transport are installed, `InitOpenTransport` starts
Apple's 68k Open Transport libraries through ASLM. Otherwise it uses
the SDK's Open Transport over MacTCP. See
[Open Transport and MacTCP](opentransport.md).

## Limits

- The SDK has the client side only. A program cannot export a
  function set or be an ASLM library.
- The SDK does not have the C++ class interfaces of ASLM (for example
  `TLibraryManager` or class sets).

## How the SDK found the interface

The SDK's interface comes from a study of how the 68k code of Open
Transport 1.3 calls ASLM. No Apple header text is in the SDK. The data
that the study found:

- ASLM's global record is at offset `0x10C` of the `ExpandMem` record.
- The library manager object is at offset `0x10` of the global record.
  It is zero when ASLM is not loaded. The routine at offset `0x64`
  loads ASLM.
- The objects use the MPW C++ object layout. The function at offset
  `0x9C` of the manager's table starts a client. The function at
  offset `0x40` of the client's table loads a function set.
- The binder at offset `0x88` of the global record binds a stub. A0
  holds the stub's slot, and D0 holds the client.

These runs passed:

| Program | System | Machine | Result |
| --- | --- | --- | --- |
| `tests/ot.c` (68k, native Open Transport through ASLM) | Mac OS 8.1, Open Transport 1.3 | QEMU `q800` | all ok |
| `examples/net/fetch.c` (68k) | Mac OS 8.1 | QEMU `q800` | Both requests got `200 OK` |
