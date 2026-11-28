/* Open Transport on PowerPC: the client calls that Apple's
 * OpenTransportAppPPC.o and OpenTptInetPPC.o supplied.
 *
 * Open Transport's shared libraries (OTClientLib, OTClientUtilLib,
 * OTUtilityLib, OTInetClientLib) export most calls by their own names.
 * The calls that open a provider, allocate memory or start Open
 * Transport are exported only as "Priv" forms, which take one more
 * argument: the address of the program's client record. This file
 * keeps that record and gives the public names (InitOpenTransport,
 * OTOpenEndpoint, ...).
 *
 * The client record is 36 bytes. The program fills three fields before
 * InitOpenTransportPriv; Open Transport uses the rest:
 *   8   the address of a long that Open Transport keeps for the program
 *   12  the address of 8 bytes that Open Transport keeps for the program
 *   16  a routine descriptor that Open Transport installs as a patch on
 *       ExitToShell; the patch calls OTExitPatchPriv, so that Open
 *       Transport closes the program's providers when it quits
 *   32  1 (a byte)
 * Apple's application glue passes 3 to InitOpenTransportPriv, and 0
 * for the utilities only.
 *
 * On 68k, ot68k.c implements the same calls over MacTCP.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#if defined(__powerpc__)
#include <Gestalt.h>
#include <MixedMode.h>
#include <Multiverse.h>
#include <OpenTransport.h>
#include <OpenTransportProviders.h>

/* The forms that Open Transport exports. They are weak: a program that
   runs without Open Transport loads, and InitOpenTransport fails. */
#define PRIV(ret, name, args) extern ret name args __asm__("__imp_" #name) __attribute__((weak))
PRIV(OSStatus, InitOpenTransportPriv, (UInt32 flags, void *client));
PRIV(void, CloseOpenTransportPriv, (void *client));
PRIV(void, OTExitPatchPriv, (void *client));
PRIV(OSStatus, OTRegisterAsClientPriv, (OTClientName name, OTNotifyUPP proc, void *client));
PRIV(OSStatus, OTUnregisterAsClientPriv, (void *client));
PRIV(void *, OTAllocMemPriv, (OTByteCount size, void *client));
PRIV(void *, OTAllocPriv,
     (EndpointRef ref, OTStructType structType, OTFieldsType fields, OSStatus *err, void *client));
PRIV(EndpointRef, OTOpenEndpointPriv,
     (OTConfigurationRef config, OTOpenFlags oflag, TEndpointInfo *info, OSStatus *err, void *client));
PRIV(OSStatus, OTAsyncOpenEndpointPriv,
     (OTConfigurationRef config, OTOpenFlags oflag, TEndpointInfo *info, OTNotifyUPP proc,
      void *contextPtr, void *client));
PRIV(OSStatus, OTCloseProviderPriv, (ProviderRef ref, void *client));
PRIV(long, OTCreateDeferredTaskPriv, (OTProcessUPP proc, void *contextInfo, void *client));
PRIV(long, OTCreateSystemTaskPriv, (OTProcessUPP proc, void *contextInfo, void *client));
PRIV(InetSvcRef, OTOpenInternetServicesPriv,
     (OTConfigurationRef cfig, OTOpenFlags oflag, OSStatus *err, void *client));
PRIV(OSStatus, OTAsyncOpenInternetServicesPriv,
     (OTConfigurationRef cfig, OTOpenFlags oflag, OTNotifyUPP proc, void *contextPtr, void *client));

/* The values that InitOpenTransportPriv takes. */
enum { INIT_UTILITIES = 0, INIT_APPLICATION = 3 };

/* The client record and the storage that its fields point to. */
static struct {
  void *info;            /* 0: Open Transport's client information */
  void *context;         /* 4 */
  long *slot;            /* 8 */
  long *data;            /* 12 */
  RoutineDescriptor *exit_patch; /* 16 */
  long reserved[3];      /* 20 */
  unsigned char flags[4]; /* 32 */
} client;
static long client_slot;
static long client_data[2];

/* The ExitToShell patch that Open Transport installs: it closes the
   program's providers. Gives 0. */
static long exit_patch(void) {
  OTExitPatchPriv(&client);
  return 0;
}

/* A routine descriptor for exit_patch: PowerPC code, a Pascal function
   with a 4-byte result and no arguments (procInfo 0x30). */
static RoutineDescriptor exit_patch_rd = {
    0xAAFE, 7, 0, 0, 0, 0, 0, {{0x30, 0, 1, 4, (ProcPtr)exit_patch, 0, 0}}};

/* Starts Open Transport with the flags that InitOpenTransportPriv
   takes. Gives kOTNotFoundErr when Open Transport is not there. */
static OSStatus start(UInt32 flags) {
  long resp;
  if (Gestalt(gestaltOpenTpt, &resp) != noErr || !(resp & gestaltOpenTptPresentMask))
    return kOTNotFoundErr;
  if (!InitOpenTransportPriv)
    return kOTNotFoundErr;
  client.slot = &client_slot;
  client.data = client_data;
  client.exit_patch = &exit_patch_rd;
  client.flags[0] = 1;
  return InitOpenTransportPriv(flags, &client);
}

/* Starts Open Transport for the program. */
OSStatus InitOpenTransport(void) { return start(INIT_APPLICATION); }

/* Starts the utility calls of Open Transport only. */
OSStatus InitOpenTransportUtilities(void) { return start(INIT_UTILITIES); }

/* Ends the program's use of Open Transport and closes its providers. */
void CloseOpenTransport(void) { CloseOpenTransportPriv(&client); }

/* Asks for the client events of Open Transport. */
OSStatus OTRegisterAsClient(OTClientName name, OTNotifyUPP proc) {
  return OTRegisterAsClientPriv(name, proc, &client);
}

/* Stops the client events. */
OSStatus OTUnregisterAsClient(void) { return OTUnregisterAsClientPriv(&client); }

/* Allocates memory from Open Transport's pool for the program. */
void *OTAllocMem(OTByteCount size) { return OTAllocMemPriv(size, &client); }

/* Allocates an XTI record with buffers of the endpoint's sizes. */
void *OTAlloc(EndpointRef ref, OTStructType structType, OTFieldsType fields, OSStatus *err) {
  return OTAllocPriv(ref, structType, fields, err, &client);
}

/* Opens an endpoint and waits until it is open. */
EndpointRef OTOpenEndpoint(OTConfigurationRef config, OTOpenFlags oflag, TEndpointInfo *info,
                           OSStatus *err) {
  return OTOpenEndpointPriv(config, oflag, info, err, &client);
}

/* Opens an endpoint; proc gets T_OPENCOMPLETE. */
OSStatus OTAsyncOpenEndpoint(OTConfigurationRef config, OTOpenFlags oflag, TEndpointInfo *info,
                             OTNotifyUPP proc, void *contextPtr) {
  return OTAsyncOpenEndpointPriv(config, oflag, info, proc, contextPtr, &client);
}

/* Closes an endpoint or other provider. */
OSStatus OTCloseProvider(ProviderRef ref) { return OTCloseProviderPriv(ref, &client); }

/* Makes a deferred task. Gives its reference, or 0. */
long OTCreateDeferredTask(OTProcessUPP proc, void *contextInfo) {
  return OTCreateDeferredTaskPriv(proc, contextInfo, &client);
}

/* Makes a system task. Gives its reference, or 0. */
long OTCreateSystemTask(OTProcessUPP proc, void *contextInfo) {
  return OTCreateSystemTaskPriv(proc, contextInfo, &client);
}

/* Opens the Internet services provider and waits until it is open. */
InetSvcRef OTOpenInternetServices(OTConfigurationRef cfig, OTOpenFlags oflag, OSStatus *err) {
  return OTOpenInternetServicesPriv(cfig, oflag, err, &client);
}

/* Opens the Internet services provider; proc gets T_OPENCOMPLETE. */
OSStatus OTAsyncOpenInternetServices(OTConfigurationRef cfig, OTOpenFlags oflag, OTNotifyUPP proc,
                                     void *contextPtr) {
  return OTAsyncOpenInternetServicesPriv(cfig, oflag, proc, contextPtr, &client);
}
#endif
