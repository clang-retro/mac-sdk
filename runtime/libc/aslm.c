/* The client side of the Apple Shared Library Manager (ASLM) on 68k.
 *
 * ASLM keeps its global record at offset 0x10C of ExpandMem. The
 * fields that a client uses:
 *   0x10  the library manager object (zero when ASLM is not loaded)
 *   0x0C  a field that the client sets to zero after it starts
 *   0x64  a routine that loads ASLM (D0 = 1) or unloads it (D0 = 2)
 *   0x88  the binder: a stub jumps to it with A0 = the stub slot and
 *         D0 = the client's local library manager
 *
 * The objects use the MPW C++ object layout: the first long is the
 * address of a table of function addresses. The functions use the C
 * calling convention, with the object as the first argument:
 *   manager 0x9C  starts a client (gives its local library manager)
 *   manager 0xA0  ends a client
 *   client  0x40  loads the library that has a function set
 *
 * The start call takes a jump buffer: when ASLM cannot make the client
 * (for example, without memory), it jumps back to the buffer with D0
 * nonzero.
 *
 * MPW code keeps only D3-D7 and A2-A4 (and A5, A6): it can change D2,
 * which clang keeps across a call. So each call to MPW code goes
 * through ASLMCallC or ASLMCallPascal, which keep D2.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#if defined(__m68k__)
#include <stdlib.h>
#include <LibraryManager.h>
#include <Multiverse.h>

/* The offsets in ASLM's global record. */
enum { ROOT_CLEAR = 0x0C, ROOT_MANAGER = 0x10, ROOT_LOADER = 0x64, ROOT_BINDER = 0x88 };

/* The offsets in the function tables. */
enum { MANAGER_START = 0x9C, MANAGER_END = 0xA0, CLIENT_LOAD = 0x40 };

char *__aslm_root;   /* ASLM's global record */
void *__aslm_client; /* the program's local library manager */
static int exit_set; /* CleanupLibraryManager is an atexit function */

long __aslm_start(void *manager, void **client, long pool, long zone, long mem, long shared);
long __aslm_loader(void *routine, long selector);
void *__aslm_auto_start(void);

/* Calls the start function of the manager with a jump buffer.
   Gives the result of the call, or the value of D0 when ASLM jumps
   back to the buffer. */
__asm__(".text\n\t.globl __aslm_start\n\t.p2align 1\n__aslm_start:\n"
        "\tlink.w %a6, #-48\n"
        "\tmovem.l %d2-%d7/%a2-%a4, -(%sp)\n"
        "\tlea (-48, %a6), %a0\n"
        "\tlea (2f, %pc), %a1\n"
        "\tmovem.l %d2-%d7/%a1-%a4/%a6-%a7, (%a0)\n"
        "\tmoveq #0, %d0\n"
        "2:\tcmpi.l #0, %d0\n"
        "\tbne 3f\n"
        "\tmove.l (28, %a6), -(%sp)\n"   /* shared */
        "\tmove.l (24, %a6), -(%sp)\n"   /* mem */
        "\tmove.l (20, %a6), -(%sp)\n"   /* zone */
        "\tmove.l (16, %a6), -(%sp)\n"   /* pool */
        "\tlea (-48, %a6), %a0\n"
        "\tmove.l %a0, -(%sp)\n"         /* the jump buffer */
        "\tmove.l (12, %a6), -(%sp)\n"   /* client */
        "\tmove.l (8, %a6), %a0\n"
        "\tmove.l %a0, -(%sp)\n"         /* the manager */
        "\tmove.l (%a0), %a0\n"
        "\tmove.l (0x9C, %a0), %a0\n"
        "\tjsr (%a0)\n"
        "\tlea (28, %sp), %sp\n"
        "\text.l %d0\n"
        "\tbra 4f\n"
        "3:\tmove.l #-3132, %d0\n"
        "4:\tmovem.l (%sp)+, %d2-%d7/%a2-%a4\n"
        "\tunlk %a6\n"
        "\trts\n");

/* Calls ASLM's load routine with the selector in D0. Keeps all
   registers. Gives D0. */
__asm__(".text\n\t.globl __aslm_loader\n\t.p2align 1\n__aslm_loader:\n"
        "\tmovem.l %d1-%d7/%a1-%a6, -(%sp)\n"
        "\tmove.l (56, %sp), %a0\n"
        "\tmove.l (60, %sp), %d0\n"
        "\tjsr (%a0)\n"
        "\tmovem.l (%sp)+, %d1-%d7/%a1-%a6\n"
        "\trts\n");

/* The binder of the stubs that ASLM_FUNCTION makes: A0 is the slot.
   It starts ASLM for the program when the program did not, then jumps
   to ASLM's binder with D0 = the program's local library manager.
   Without ASLM it gives kASLMNotPresentErr. */
__asm__(".text\n\t.globl __aslm_bind\n\t.p2align 1\n__aslm_bind:\n"
        "\tmove.l __aslm_client, %d0\n"
        "\tbne 1f\n"
        "\tmove.l %a0, -(%sp)\n"
        "\tjsr __aslm_auto_start\n"
        "\tmove.l (%sp)+, %a0\n"
        "\tcmpi.l #0, %d0\n"
        "\tbeq 2f\n"
        "1:\tmove.l __aslm_root, %a1\n"
        "\tmove.l (0x88, %a1), %a1\n"
        "\tjmp (%a1)\n"
        "2:\tmove.l #-3132, %d0\n"
        "\trts\n");

/* Calls a function of the MPW C calling convention: see
   LibraryManager.h. __mac_mpw_call is the name for the rest of the C
   library. */
__asm__(".text\n\t.globl ASLMCallC\n\t.globl __mac_mpw_call\n\t.p2align 1\n"
        "ASLMCallC:\n__mac_mpw_call:\n"
        "\tlink.w %a6, #0\n"
        "\tmovem.l %d2-%d7/%a2-%a4, -(%sp)\n"
        "\tmove.l (8, %a6), %a2\n"       /* fn */
        "\tmove.l (12, %a6), %d2\n"      /* nargs */
        "\tmove.l (16, %a6), %a3\n"      /* args */
        "\tmove.l %d2, %d3\n"
        "\tlsl.l #2, %d3\n"
        "\tadda.l %d3, %a3\n"            /* after the last argument */
        "\tmoveq #0, %d4\n"
        "1:\tcmp.l %d2, %d4\n"
        "\tbge 2f\n"
        "\tmove.l -(%a3), -(%sp)\n"
        "\tadd.l #1, %d4\n"
        "\tbra 1b\n"
        "2:\tjsr (%a2)\n"
        "\tmove.l (12, %a6), %d2\n"
        "\tlsl.l #2, %d2\n"
        "\tadda.l %d2, %sp\n"
        "\tmovem.l (%sp)+, %d2-%d7/%a2-%a4\n"
        "\tunlk %a6\n"
        "\trts\n");

/* Calls a Pascal function: see LibraryManager.h. */
__asm__(".text\n\t.globl ASLMCallPascal\n\t.p2align 1\nASLMCallPascal:\n"
        "\tlink.w %a6, #0\n"
        "\tmovem.l %d2-%d7/%a2-%a4, -(%sp)\n"
        "\tmove.l (8, %a6), %a2\n"       /* fn */
        "\tmove.l (12, %a6), %d2\n"      /* nargs */
        "\tmove.l (16, %a6), %d3\n"      /* wordMask */
        "\tmove.l (20, %a6), %d4\n"      /* resultSize */
        "\tmove.l (24, %a6), %a3\n"      /* args */
        "\tcmpi.l #2, %d4\n"
        "\tbeq 1f\n"
        "\tcmpi.l #4, %d4\n"
        "\tbne 2f\n"
        "\tclr.l -(%sp)\n"
        "\tbra 2f\n"
        "1:\tclr.w -(%sp)\n"
        "2:\tmoveq #0, %d5\n"
        "3:\tcmp.l %d2, %d5\n"
        "\tbge 6f\n"
        "\tmove.l (%a3)+, %d0\n"
        "\tbtst %d5, %d3\n"
        "\tbeq 4f\n"
        "\tmove.w %d0, -(%sp)\n"
        "\tbra 5f\n"
        "4:\tmove.l %d0, -(%sp)\n"
        "5:\tadd.l #1, %d5\n"
        "\tbra 3b\n"
        "6:\tjsr (%a2)\n"
        "\tmoveq #0, %d0\n"
        "\tcmpi.l #2, %d4\n"
        "\tbne 7f\n"
        "\tmove.w (%sp)+, %d0\n"
        "\tbra 8f\n"
        "7:\tcmpi.l #4, %d4\n"
        "\tbne 8f\n"
        "\tmove.l (%sp)+, %d0\n"
        "8:\tmovem.l (%sp)+, %d2-%d7/%a2-%a4\n"
        "\tunlk %a6\n"
        "\trts\n");

/* Gives the address of a function in the function table of an MPW
   C++ object. */
static void *method(void *object, int offset) {
  return *(void **)(*(char **)object + offset);
}

/* Gives ASLM's global record, or NULL when the system has none. */
static char *find_root(void) {
  char *em = *(char **)0x2B6; /* ExpandMem */
  if (!em || GetPtrSize(em) < 0x110)
    return NULL;
  return *(char **)(em + 0x10C);
}

/* Gives true when ASLM is installed. Loads ASLM when it is installed
   but not loaded. */
Boolean ASLMPresent(void) {
  char *root = find_root();
  void *loader;
  if (!root)
    return false;
  if (*(long *)(root + ROOT_MANAGER))
    return true;
  loader = *(void **)(root + ROOT_LOADER);
  if (loader)
    __aslm_loader(loader, 1);
  return *(long *)(root + ROOT_MANAGER) != 0;
}

/* Gives the value of A5. */
static long current_a5(void) {
  long a5;
  __asm__("move.l %%a5, %0" : "=d"(a5));
  return a5;
}

/* Starts the program's use of ASLM. */
OSErr InitLibraryManager(size_t poolSize, int zoneType, int memType) {
  void *manager;
  long rc;
  if (__aslm_client)
    return noErr;
  if (!ASLMPresent())
    return kASLMNotPresentErr;
  __aslm_root = find_root();
  manager = *(void **)(__aslm_root + ROOT_MANAGER);
  rc = __aslm_start(manager, &__aslm_client, (long)poolSize, zoneType, memType, 0);
  if (rc != noErr || !__aslm_client) {
    __aslm_client = NULL;
    return rc ? (OSErr)rc : kASLMNotPresentErr;
  }
  /* Apple's glue does this for an application (A5 is CurrentA5). */
  if (current_a5() == *(long *)0x904)
    *(long *)(__aslm_root + ROOT_CLEAR) = 0;
  if (!exit_set) {
    exit_set = 1;
    atexit(CleanupLibraryManager);
  }
  return noErr;
}

/* Starts ASLM with the default values for the first call of a stub.
   Gives the program's local library manager, or NULL. */
void *__aslm_auto_start(void) {
  InitLibraryManager(kASLMDefaultPoolSize, kASLMDefaultZone, kASLMDefaultMemory);
  return __aslm_client;
}

/* Ends the program's use of ASLM. */
void CleanupLibraryManager(void) {
  void *manager, *loader;
  unsigned char unload;
  long args[3];
  if (!__aslm_client)
    return;
  manager = *(void **)(__aslm_root + ROOT_MANAGER);
  args[0] = (long)manager;
  args[1] = (long)&__aslm_client;
  args[2] = 0;
  unload = (unsigned char)ASLMCallC(method(manager, MANAGER_END), 3, args);
  __aslm_client = NULL;
  loader = *(void **)(__aslm_root + ROOT_LOADER);
  if (unload && loader)
    __aslm_loader(loader, 2);
}

/* Loads the library that has a function set. */
OSErr ASLMLoadFunctionSet(const char *id) {
  long args[3];
  if (!__aslm_client &&
      InitLibraryManager(kASLMDefaultPoolSize, kASLMDefaultZone, kASLMDefaultMemory) != noErr)
    return kASLMNotPresentErr;
  args[0] = (long)__aslm_client;
  args[1] = (long)id;
  args[2] = 1;
  return (OSErr)ASLMCallC(method(__aslm_client, CLIENT_LOAD), 3, args);
}
#endif
