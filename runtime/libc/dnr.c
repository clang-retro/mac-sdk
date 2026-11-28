/* The MacTCP name resolver (DNR): OpenResolver, StrToAddr, AddrToName
 * and the other calls of AddressXlation.h.
 *
 * The resolver is a 68k code resource ('dnrp'). MacTCP keeps it in its
 * control panel; Open Transport keeps it in the System file. This file
 * finds the resource, keeps it locked in the heap, and calls it with a
 * selector and the arguments of the call. On PowerPC the calls go
 * through the Mixed Mode Manager. netdb.c uses the same calls for
 * getaddrinfo and the other name functions.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <stdlib.h>
#include <string.h>
#include <Files.h>
#include <Folders.h>
#include <MacMemory.h>
#include <MixedMode.h>
#include <Multiverse.h>
#include <Resources.h>
#include <AddressXlation.h>
#include "socket.h"

/* The selectors of the resolver's entry point. */
enum {
  OPENRESOLVER = 1,
  CLOSERESOLVER = 2,
  STRTOADDR = 3,
  ADDRTOSTR = 4,
  ENUMCACHE = 5,
  ADDRTONAME = 6,
  HXINFO = 7,
  MXINFO = 8
};

/* A Pascal result procedure in 68k code that does nothing: it takes
   its two arguments off the stack. A caller without a result procedure
   examines rtnCode.
     movea.l (sp)+,a0 ; addq.l #8,sp ; jmp (a0) */
static const unsigned short no_result[] = {0x205F, 0x508F, 0x4ED0};

static Handle dnr; /* the resolver's code, locked; NULL when closed */

/* Calls the resolver with a selector and up to four long arguments.
   Gives the resolver's result. */
static long dnr_call(long sel, long a, long b, long c, long d) {
#if defined(__powerpc__)
  /* C on the stack, a 4-byte result, five 4-byte arguments. */
  const ProcInfoType info = 1 | (3 << 4) | (3 << 6) | (3 << 8) | (3 << 10) | (3 << 12) | (3 << 14);
  return CallUniversalProc((UniversalProcPtr)*dnr, info, sel, a, b, c, d);
#else
  typedef long (*proc)(long, long, long, long, long);
  return ((proc)*dnr)(sel, a, b, c, d);
#endif
}

/* Looks in one folder for the MacTCP control panel (type 'cdev',
   creator 'ztcp' or 'mtcp') and opens its resource fork. Gives its
   reference number, or -1. */
static short open_panel(short vol, long dir) {
  CInfoPBRec pb;
  Str255 name;
  short i;
  for (i = 1;; i++) {
    OSType type, creator;
    memset(&pb, 0, sizeof pb);
    pb.hFileInfo.ioNamePtr = name;
    pb.hFileInfo.ioVRefNum = vol;
    pb.hFileInfo.ioDirID = dir;
    pb.hFileInfo.ioFDirIndex = i;
    if (PBGetCatInfoSync(&pb) != noErr)
      return -1;
    if (pb.hFileInfo.ioFlAttrib & 0x10)
      continue;
    type = pb.hFileInfo.ioFlFndrInfo.fdType;
    creator = pb.hFileInfo.ioFlFndrInfo.fdCreator;
    if (type == 'cdev' && (creator == 'ztcp' || creator == 'mtcp')) {
      short ref = HOpenResFile(vol, dir, name, fsRdPerm);
      if (ref != -1)
        return ref;
    }
  }
}

/* Finds the resolver's code and detaches it from its file. Gives the
   handle, or NULL. */
static Handle find_resolver(void) {
  static const OSType folders[] = {kControlPanelFolderType, kExtensionFolderType, kSystemFolderType};
  short ref = -1, vol;
  long dir;
  unsigned i;
  Handle h;
  for (i = 0; ref == -1 && i < sizeof folders / sizeof folders[0]; i++)
    if (FindFolder(kOnSystemDisk, folders[i], 0, &vol, &dir) == noErr)
      ref = open_panel(vol, dir);
  /* Open Transport keeps the resolver in the System file. */
  h = ref != -1 ? Get1IndResource('dnrp', 1) : 0;
  if (!h)
    h = GetIndResource('dnrp', 1);
  if (h)
    DetachResource(h);
  if (ref != -1)
    CloseResFile(ref);
  return h;
}

/* Closes the resolver when the program ends. */
static void close_at_exit(void) { CloseResolver(); }

/* Loads and opens the resolver. Gives noErr when it is open (also when
   it was open before), or an error. */
OSErr OpenResolver(char *fileName) {
  static int registered;
  long rc;
  (void)fileName;
  if (dnr)
    return noErr;
  if (__mac_ip_address() == 0)
    return ipNoCnfgErr;
  dnr = find_resolver();
  if (!dnr)
    return resNotFound;
  HLock(dnr);
  rc = dnr_call(OPENRESOLVER, 0, 0, 0, 0);
  if (rc != noErr) {
    DisposeHandle(dnr);
    dnr = NULL;
    return (OSErr)rc;
  }
  if (!registered) {
    registered = 1;
    atexit(close_at_exit);
  }
  return noErr;
}

/* Closes the resolver and frees its code. Gives noErr, or notOpenErr
   when it was not open. */
OSErr CloseResolver(void) {
  if (!dnr)
    return notOpenErr;
  dnr_call(CLOSERESOLVER, 0, 0, 0, 0);
  DisposeHandle(dnr);
  dnr = NULL;
  return noErr;
}

/* Gives the result procedure to pass: proc, or the one that does
   nothing when proc is NULL. */
static long result_proc(void *proc) { return proc ? (long)proc : (long)no_result; }

/* Finds the addresses of a host name or a dotted number. */
OSErr StrToAddr(char *hostName, hostInfo *rtnStruct, ResultUPP resultproc, char *userDataPtr) {
  if (!dnr)
    return notOpenErr;
  return (OSErr)dnr_call(STRTOADDR, (long)hostName, (long)rtnStruct, result_proc((void *)resultproc),
                         (long)userDataPtr);
}

/* Writes an address as a dotted number. */
OSErr AddrToStr(unsigned long addr, char *addrStr) {
  if (!dnr)
    return notOpenErr;
  return (OSErr)dnr_call(ADDRTOSTR, (long)addr, (long)addrStr, 0, 0);
}

/* Calls resultproc for each entry of the resolver's cache. */
OSErr EnumCache(EnumResultUPP resultproc, char *userDataPtr) {
  if (!dnr)
    return notOpenErr;
  return (OSErr)dnr_call(ENUMCACHE, result_proc((void *)resultproc), (long)userDataPtr, 0, 0);
}

/* Finds the name of an address. */
OSErr AddrToName(ip_addr addr, hostInfo *rtnStruct, ResultUPP resultproc, char *userDataPtr) {
  if (!dnr)
    return notOpenErr;
  return (OSErr)dnr_call(ADDRTONAME, (long)addr, (long)rtnStruct, result_proc((void *)resultproc),
                         (long)userDataPtr);
}

/* Finds the host information record of a name. */
OSErr HInfo(char *hostName, returnRec *returnRecPtr, ResultProc2UPP resultProc, char *userDataPtr) {
  if (!dnr)
    return notOpenErr;
  return (OSErr)dnr_call(HXINFO, (long)hostName, (long)returnRecPtr, result_proc((void *)resultProc),
                         (long)userDataPtr);
}

/* Finds the mail exchange record of a name. */
OSErr MXInfo(char *hostName, returnRec *returnRecPtr, ResultProc2UPP resultProc, char *userDataPtr) {
  if (!dnr)
    return notOpenErr;
  return (OSErr)dnr_call(MXINFO, (long)hostName, (long)returnRecPtr, result_proc((void *)resultProc),
                         (long)userDataPtr);
}
