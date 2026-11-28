/* Open Transport on 68k: the calls to native Open Transport.
 *
 * Open Transport's 68k client code is in Apple Shared Library Manager
 * (ASLM) libraries. Three function sets have the client calls:
 *   "OTClnt$LclUtils"      start and end, memory, configurations
 *   "OTClnt$LclClnt"       providers and endpoints
 *   "ot:inet$LclInetClnt"  the Internet services
 * The stubs below bind to them through aslm.c. The number of a stub is
 * the place of the function in the export table of its set (Open
 * Transport 1.1 to 1.3 have the same tables).
 *
 * InitOpenTransport starts native Open Transport when ASLM and Open
 * Transport are installed. Then each public call goes to the native
 * function. Else InitOpenTransport starts the MacTCP form (ot68k.c),
 * and each public call goes to the __otsock_ form.
 *
 * The functions with names in capitals use the Pascal calling
 * convention: ASLMCallPascal calls them. The others use MPW's C calling
 * convention: ASLMCallC calls them, because MPW code can change D2 and
 * gives a pointer result in D0 (clang expects A0).
 *
 * Native Open Transport calls a notifier with the Pascal calling
 * convention. The SDK's notifiers use the C calling convention, so the
 * library gives Open Transport a Pascal trampoline and a slot that
 * holds the program's notifier and context.
 *
 * The client record (36 bytes) is the same as on PowerPC (ot.c), with
 * these 68k values, as Apple's application glue sets them:
 *   8   the address of the program's ASLM client (aslm.c)
 *   12  the address of ASLM's global record (aslm.c)
 *   28  1 when the glue started ASLM
 *   30  1 when the glue loaded "OTClnt$LclUtils"
 *   32  1 for an application
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#if defined(__m68k__)
#include <Gestalt.h>
#include <LibraryManager.h>
#include <Multiverse.h>
#include <OpenTransport.h>
#include <OpenTransportProviders.h>
#include "ot68k-names.h"

/* The MacTCP forms (ot68k.c). */
#define SOCK_FORM(name) extern __typeof__(name) __otsock_##name;
OT68K_CALLS(SOCK_FORM)

extern void *__aslm_client;
extern char *__aslm_root;

/* The function sets, with the versions that Apple's glue asks for. */
static ASLM_FUNCTION_SET(utils, "OTClnt$LclUtils", 0x0100, 0x0100);
static ASLM_FUNCTION_SET(clnt, "OTClnt$LclClnt", 0x0100, 0x0100);
static ASLM_FUNCTION_SET(inet, "ot:inet$LclInetClnt", 0x0300, 0x0300);

/* "OTClnt$LclUtils" */
ASLM_FUNCTION(__otn_AllocMemPriv, utils, 1);
ASLM_FUNCTION(__otn_InitPriv, utils, 3);
ASLM_FUNCTION(__otn_ClosePriv, utils, 4);
ASLM_FUNCTION(__otn_RegisterAsClientPriv, utils, 5);
ASLM_FUNCTION(__otn_UnregisterAsClientPriv, utils, 6);
ASLM_FUNCTION(__otn_CREATECONFIGURATION, utils, 16);
ASLM_FUNCTION(__otn_DESTROYCONFIGURATION, utils, 17);
ASLM_FUNCTION(__otn_CLONECONFIGURATION, utils, 18);
ASLM_FUNCTION(__otn_FreeMem, utils, 31);
/* "OTClnt$LclClnt" */
ASLM_FUNCTION(__otn_IDLE, clnt, 1);
ASLM_FUNCTION(__otn_CloseProviderPriv, clnt, 9);
ASLM_FUNCTION(__otn_SETNONBLOCKING, clnt, 10);
ASLM_FUNCTION(__otn_SETBLOCKING, clnt, 11);
ASLM_FUNCTION(__otn_ISBLOCKING, clnt, 12);
ASLM_FUNCTION(__otn_SETSYNCHRONOUS, clnt, 13);
ASLM_FUNCTION(__otn_SETASYNCHRONOUS, clnt, 14);
ASLM_FUNCTION(__otn_ISSYNCHRONOUS, clnt, 15);
ASLM_FUNCTION(__otn_DONTACKSENDS, clnt, 16);
ASLM_FUNCTION(__otn_ACKSENDS, clnt, 17);
ASLM_FUNCTION(__otn_ISACKINGSENDS, clnt, 18);
ASLM_FUNCTION(__otn_CANCELSYNCHRONOUSCALLS, clnt, 19);
ASLM_FUNCTION(__otn_INSTALLNOTIFIER, clnt, 20);
ASLM_FUNCTION(__otn_REMOVENOTIFIER, clnt, 21);
ASLM_FUNCTION(__otn_IOCTL, clnt, 22);
ASLM_FUNCTION(__otn_OpenEndpointPriv, clnt, 29);
ASLM_FUNCTION(__otn_AsyncOpenEndpointPriv, clnt, 30);
ASLM_FUNCTION(__otn_GETENDPOINTINFO, clnt, 31);
ASLM_FUNCTION(__otn_GETPROTADDRESS, clnt, 32);
ASLM_FUNCTION(__otn_RESOLVEADDRESS, clnt, 33);
ASLM_FUNCTION(__otn_GETENDPOINTSTATE, clnt, 34);
ASLM_FUNCTION(__otn_AllocPriv, clnt, 35);
ASLM_FUNCTION(__otn_FREE, clnt, 36);
ASLM_FUNCTION(__otn_LOOK, clnt, 37);
ASLM_FUNCTION(__otn_COUNTDATABYTES, clnt, 39);
ASLM_FUNCTION(__otn_OPTIONMANAGEMENT, clnt, 40);
ASLM_FUNCTION(__otn_BIND, clnt, 41);
ASLM_FUNCTION(__otn_UNBIND, clnt, 42);
ASLM_FUNCTION(__otn_CONNECT, clnt, 43);
ASLM_FUNCTION(__otn_RCVCONNECT, clnt, 44);
ASLM_FUNCTION(__otn_LISTEN, clnt, 45);
ASLM_FUNCTION(__otn_ACCEPT, clnt, 46);
ASLM_FUNCTION(__otn_SNDDISCONNECT, clnt, 47);
ASLM_FUNCTION(__otn_SNDORDERLYDISCONNECT, clnt, 48);
ASLM_FUNCTION(__otn_RCVDISCONNECT, clnt, 49);
ASLM_FUNCTION(__otn_RCVORDERLYDISCONNECT, clnt, 50);
ASLM_FUNCTION(__otn_SND, clnt, 51);
ASLM_FUNCTION(__otn_RCV, clnt, 52);
ASLM_FUNCTION(__otn_SNDUDATA, clnt, 53);
ASLM_FUNCTION(__otn_RCVUDATA, clnt, 54);
ASLM_FUNCTION(__otn_RCVUDERR, clnt, 55);
ASLM_FUNCTION(__otn_GETPROVIDERPORTREF, clnt, 72);
ASLM_FUNCTION(__otn_USESYNCIDLEEVENTS, clnt, 74);
ASLM_FUNCTION(__otn_ENTERNOTIFIER, clnt, 75);
ASLM_FUNCTION(__otn_LEAVENOTIFIER, clnt, 76);
/* "ot:inet$LclInetClnt" */
ASLM_FUNCTION(__otn_AsyncOpenInternetServicesPriv, inet, 1);
ASLM_FUNCTION(__otn_OpenInternetServicesPriv, inet, 2);
ASLM_FUNCTION(__otn_INETSTRINGTOADDRESS, inet, 3);
ASLM_FUNCTION(__otn_INETADDRESSTONAME, inet, 4);
ASLM_FUNCTION(__otn_INETSYSINFO, inet, 5);
ASLM_FUNCTION(__otn_INETMAILEXCHANGE, inet, 6);
ASLM_FUNCTION(__otn_INETQUERY, inet, 12);

/* The C functions: ASLMCallC calls them. */
extern const char __otn_AllocMemPriv[], __otn_InitPriv[], __otn_ClosePriv[],
    __otn_RegisterAsClientPriv[], __otn_UnregisterAsClientPriv[], __otn_FreeMem[],
    __otn_CloseProviderPriv[], __otn_OpenEndpointPriv[], __otn_AsyncOpenEndpointPriv[],
    __otn_AllocPriv[], __otn_AsyncOpenInternetServicesPriv[], __otn_OpenInternetServicesPriv[];

/* Calls the C function fn of MPW. */
#define CCALL(fn, ...)                                                                             \
  ({                                                                                               \
    const long args_[] = {__VA_ARGS__};                                                            \
    ASLMCallC(fn, (int)(sizeof args_ / sizeof args_[0]), args_);                                   \
  })

/* The Pascal functions: ASLMCallPascal calls them. */
extern const char __otn_CREATECONFIGURATION[], __otn_DESTROYCONFIGURATION[],
    __otn_CLONECONFIGURATION[], __otn_IDLE[], __otn_SETNONBLOCKING[], __otn_SETBLOCKING[],
    __otn_ISBLOCKING[], __otn_SETSYNCHRONOUS[], __otn_SETASYNCHRONOUS[], __otn_ISSYNCHRONOUS[],
    __otn_DONTACKSENDS[], __otn_ACKSENDS[], __otn_ISACKINGSENDS[],
    __otn_CANCELSYNCHRONOUSCALLS[], __otn_INSTALLNOTIFIER[], __otn_REMOVENOTIFIER[],
    __otn_IOCTL[], __otn_GETENDPOINTINFO[], __otn_GETPROTADDRESS[], __otn_RESOLVEADDRESS[],
    __otn_GETENDPOINTSTATE[], __otn_FREE[], __otn_LOOK[], __otn_COUNTDATABYTES[],
    __otn_OPTIONMANAGEMENT[], __otn_BIND[], __otn_UNBIND[], __otn_CONNECT[], __otn_RCVCONNECT[],
    __otn_LISTEN[], __otn_ACCEPT[], __otn_SNDDISCONNECT[], __otn_SNDORDERLYDISCONNECT[],
    __otn_RCVDISCONNECT[], __otn_RCVORDERLYDISCONNECT[], __otn_SND[], __otn_RCV[],
    __otn_SNDUDATA[], __otn_RCVUDATA[], __otn_RCVUDERR[], __otn_GETPROVIDERPORTREF[],
    __otn_USESYNCIDLEEVENTS[], __otn_ENTERNOTIFIER[], __otn_LEAVENOTIFIER[],
    __otn_INETSTRINGTOADDRESS[], __otn_INETADDRESSTONAME[], __otn_INETSYSINFO[],
    __otn_INETMAILEXCHANGE[], __otn_INETQUERY[];

/* Calls the Pascal function fn with a result of rsize bytes (0, 2 or
   4). mask has a bit for each 2-byte argument. */
#define PCALL(fn, rsize, mask, ...)                                                                \
  ({                                                                                               \
    const long args_[] = {__VA_ARGS__};                                                            \
    ASLMCallPascal(fn, (int)(sizeof args_ / sizeof args_[0]), mask, rsize, args_);                 \
  })

/* Calls the Pascal function fn with one 4-byte argument. */
#define PCALL1(fn, rsize, a) PCALL(fn, rsize, 0, (long)(a))

/* Gives the Boolean in a 2-byte Pascal result. */
#define PBOOL(r) ((Boolean)(((r) >> 8) & 0xFF) != 0)

/* The values that InitOpenTransportPriv takes. */
enum { INIT_UTILITIES = 0, INIT_APPLICATION = 3 };

/* The client record. */
static struct {
  void *info;              /* 0: Open Transport's client information */
  void *context;           /* 4 */
  void **client;           /* 8 */
  char **root;             /* 12 */
  long reserved[3];        /* 16 */
  unsigned char flags[8];  /* 28 */
} rec;

static int native;         /* native Open Transport is in use */
static int started_aslm;   /* this file started ASLM */

/* ------------------------------------------------------------------ */
/* Notifiers */

/* A slot that holds a notifier of the program. The context that Open
   Transport gives the trampoline is the address of the slot. */
struct nslot {
  OTNotifyProcPtr proc;
  void *context;
  ProviderRef ref;   /* the provider, when it is known */
  int used;
};

#define NSLOTS 64
static struct nslot nslots[NSLOTS];
static OTNotifyProcPtr client_proc; /* the notifier of OTRegisterAsClient */

void __otn_trampoline(void);
void __otn_client_trampoline(void);
void __otn_notify(struct nslot *s, OTEventCode code, OTResult result, void *cookie);
void __otn_client_notify(void *context, OTEventCode code, OTResult result, void *cookie);

/* The Pascal trampolines. Open Transport calls them with (contextPtr,
   code, result, cookie) and expects them to remove 16 bytes of
   arguments. They call __otn_notify or __otn_client_notify with the
   C calling convention. */
__asm__(".text\n\t.p2align 1\n\t.globl __otn_trampoline\n__otn_trampoline:\n"
        "\tlea __otn_notify, %a1\n"
        "\tbra 1f\n"
        "\t.globl __otn_client_trampoline\n__otn_client_trampoline:\n"
        "\tlea __otn_client_notify, %a1\n"
        "1:\tmove.l (4, %sp), -(%sp)\n"    /* cookie */
        "\tmove.l (12, %sp), -(%sp)\n"     /* result */
        "\tmove.l (20, %sp), -(%sp)\n"     /* code */
        "\tmove.l (28, %sp), -(%sp)\n"     /* contextPtr */
        "\tjsr (%a1)\n"
        "\tlea (16, %sp), %sp\n"
        "\tmove.l (%sp)+, %a0\n"
        "\tlea (16, %sp), %sp\n"
        "\tjmp (%a0)\n");

/* Calls the program's notifier of a slot. When an asynchronous open
   ends, the slot learns its provider. */
void __otn_notify(struct nslot *s, OTEventCode code, OTResult result, void *cookie) {
  if ((code == T_OPENCOMPLETE) && result == kOTNoError && !s->ref)
    s->ref = (ProviderRef)cookie;
  if (s->proc)
    s->proc(s->context, code, result, cookie);
}

/* Calls the program's client notifier. */
void __otn_client_notify(void *context, OTEventCode code, OTResult result, void *cookie) {
  if (client_proc)
    client_proc(context, code, result, cookie);
}

/* Masks the interrupts and gives the old status register. */
static inline short disable(void) {
  register short sr __asm__("d0");
  __asm__ volatile(".short 0x40C0, 0x007C, 0x0700" : "=r"(sr) : : "memory");
  return sr;
}

/* Puts back the status register that disable gave. */
static inline void restore(short old) {
  register short sr __asm__("d0") = old;
  __asm__ volatile(".short 0x46C0" : : "r"(sr) : "memory");
}

/* Gives a slot for a notifier, or NULL when all slots are in use. */
static struct nslot *new_slot(OTNotifyUPP proc, void *context, ProviderRef ref) {
  struct nslot *found = NULL;
  short sr = disable();
  int i;
  for (i = 0; i < NSLOTS && !found; i++)
    if (!nslots[i].used) {
      found = &nslots[i];
      found->used = 1;
    }
  restore(sr);
  if (found) {
    found->proc = (OTNotifyProcPtr)proc;
    found->context = context;
    found->ref = ref;
  }
  return found;
}

/* Frees the slots of a provider. */
static void free_slots(ProviderRef ref) {
  int i;
  for (i = 0; i < NSLOTS; i++)
    if (nslots[i].used && nslots[i].ref == ref)
      nslots[i].used = 0;
}

/* ------------------------------------------------------------------ */
/* Start and end */

/* Starts native Open Transport with the flags of InitOpenTransportPriv.
   Gives kOTNotFoundErr when ASLM or Open Transport is not there. */
static OSStatus start_native(long flags) {
  long resp;
  OSStatus err;
  if (native)
    return kOTNoError;
  if (Gestalt(gestaltOpenTpt, &resp) != noErr || !(resp & gestaltOpenTptPresentMask))
    return kOTNotFoundErr;
  if (!ASLMPresent())
    return kOTNotFoundErr;
  if (!__aslm_client) {
    /* Apple's application glue: a 1 KB pool in the application zone. */
    if (InitLibraryManager(kASLMDefaultPoolSize, 3, kASLMDefaultMemory) != noErr)
      return kOTNotFoundErr;
    started_aslm = 1;
  }
  rec.client = &__aslm_client;
  rec.root = &__aslm_root;
  rec.flags[0] = (unsigned char)started_aslm; /* 28 */
  rec.flags[4] = 1;                           /* 32: an application */
  if (ASLMLoadFunctionSet("OTClnt$LclUtils") != noErr)
    return kOTNotFoundErr;
  rec.flags[2] = 1; /* 30 */
  err = (OSStatus)CCALL(__otn_InitPriv, flags, (long)&rec);
  if (err == kOTNoError)
    native = 1;
  return err;
}

/* Gives 1 when the calls go to native Open Transport, 0 when they go
   to the MacTCP form. For the tests. */
int __ot68k_native(void) { return native; }

/* Starts Open Transport for the program: native Open Transport when it
   is installed, else Open Transport over MacTCP. */
OSStatus InitOpenTransport(void) {
  if (start_native(INIT_APPLICATION) == kOTNoError)
    return kOTNoError;
  return __otsock_InitOpenTransport();
}

/* Starts the utility calls only. */
OSStatus InitOpenTransportUtilities(void) {
  if (start_native(INIT_UTILITIES) == kOTNoError)
    return kOTNoError;
  return __otsock_InitOpenTransportUtilities();
}

/* Ends Open Transport for the program and closes its providers. */
void CloseOpenTransport(void) {
  if (native) {
    CCALL(__otn_ClosePriv, (long)&rec);
    native = 0;
    return;
  }
  __otsock_CloseOpenTransport();
}

/* Asks for the client events of Open Transport. */
OSStatus OTRegisterAsClient(OTClientName name, OTNotifyUPP proc) {
  if (!native)
    return __otsock_OTRegisterAsClient(name, proc);
  client_proc = (OTNotifyProcPtr)proc;
  return (OSStatus)CCALL(__otn_RegisterAsClientPriv, (long)name, (long)__otn_client_trampoline,
                         (long)&rec);
}

/* Stops the client events. */
OSStatus OTUnregisterAsClient(void) {
  if (!native)
    return __otsock_OTUnregisterAsClient();
  client_proc = NULL;
  return (OSStatus)CCALL(__otn_UnregisterAsClientPriv, (long)&rec);
}

/* ------------------------------------------------------------------ */
/* Memory, configurations and idle time */

/* Allocates memory; a notifier can call it. */
void *OTAllocMem(OTByteCount size) {
  return native ? (void *)CCALL(__otn_AllocMemPriv, (long)size, (long)&rec) : __otsock_OTAllocMem(size);
}

/* Frees memory from OTAllocMem. */
void OTFreeMem(void *mem) {
  if (native)
    CCALL(__otn_FreeMem, (long)mem);
  else
    __otsock_OTFreeMem(mem);
}

/* Allocates an XTI record with buffers of the endpoint's sizes. */
void *OTAlloc(EndpointRef ref, OTStructType structType, OTFieldsType fields, OSStatus *err) {
  if (!native)
    return __otsock_OTAlloc(ref, structType, fields, err);
  return (void *)CCALL(__otn_AllocPriv, (long)ref, (long)structType, (long)fields, (long)err,
                        (long)&rec);
}

/* Frees a record from OTAlloc. */
OTResult OTFree(void *ptr, OTStructType structType) {
  if (!native)
    return __otsock_OTFree(ptr, structType);
  return (OTResult)PCALL(__otn_FREE, 4, 0, (long)ptr, (long)structType);
}

/* Lets the notifiers and the other programs run. Also runs the
   deferred and system tasks (ot68k.c). */
void OTIdle(void) {
  if (native)
    ASLMCallPascal(__otn_IDLE, 0, 0, 0, NULL);
  __otsock_OTIdle();
}

/* Makes a configuration from a path such as "tcp". */
OTConfigurationRef OTCreateConfiguration(const char *path) {
  if (!native)
    return __otsock_OTCreateConfiguration(path);
  return (OTConfigurationRef)PCALL1(__otn_CREATECONFIGURATION, 4, path);
}

/* Copies a configuration. */
OTConfigurationRef OTCloneConfiguration(OTConfigurationRef cfig) {
  if (!native)
    return __otsock_OTCloneConfiguration(cfig);
  return (OTConfigurationRef)PCALL1(__otn_CLONECONFIGURATION, 4, cfig);
}

/* Frees a configuration that no open call used. */
void OTDestroyConfiguration(OTConfigurationRef cfig) {
  if (!native)
    __otsock_OTDestroyConfiguration(cfig);
  else
    PCALL1(__otn_DESTROYCONFIGURATION, 0, cfig);
}

/* ------------------------------------------------------------------ */
/* Providers */

/* Opens an endpoint and waits until it is open. */
EndpointRef OTOpenEndpoint(OTConfigurationRef config, OTOpenFlags oflag, TEndpointInfo *info,
                           OSStatus *err) {
  OSStatus e = kOTNoError;
  EndpointRef ref;
  if (!native)
    return __otsock_OTOpenEndpoint(config, oflag, info, err);
  ref = (EndpointRef)CCALL(__otn_OpenEndpointPriv, (long)config, (long)oflag, (long)info,
                            (long)&e, (long)&rec);
  if (err)
    *err = e;
  return ref;
}

/* Opens an endpoint; proc gets T_OPENCOMPLETE. */
OSStatus OTAsyncOpenEndpoint(OTConfigurationRef config, OTOpenFlags oflag, TEndpointInfo *info,
                             OTNotifyUPP proc, void *contextPtr) {
  struct nslot *s;
  OSStatus err;
  if (!native)
    return __otsock_OTAsyncOpenEndpoint(config, oflag, info, proc, contextPtr);
  s = new_slot(proc, contextPtr, NULL);
  if (!s)
    return kOTOutOfMemoryErr;
  err = (OSStatus)CCALL(__otn_AsyncOpenEndpointPriv, (long)config, (long)oflag, (long)info,
                        (long)__otn_trampoline, (long)s, (long)&rec);
  if (err != kOTNoError)
    s->used = 0;
  return err;
}

/* Gives the configuration of the Internet services: the 68k library
   does not take kDefaultInternetServicesPath, so make "dnr". */
static OTConfigurationRef inet_config(OTConfigurationRef cfig) {
  if (cfig == kDefaultInternetServicesPath)
    return OTCreateConfiguration(kDNRName);
  return cfig;
}

/* Opens the Internet services and waits until they are open. */
InetSvcRef OTOpenInternetServices(OTConfigurationRef cfig, OTOpenFlags oflag, OSStatus *err) {
  OSStatus e = kOTNoError;
  InetSvcRef ref;
  if (!native)
    return __otsock_OTOpenInternetServices(cfig, oflag, err);
  cfig = inet_config(cfig);
  ref = (InetSvcRef)CCALL(__otn_OpenInternetServicesPriv, (long)cfig, (long)oflag, (long)&e,
                          (long)&rec);
  if (err)
    *err = e;
  return ref;
}

/* Opens the Internet services; proc gets T_OPENCOMPLETE. */
OSStatus OTAsyncOpenInternetServices(OTConfigurationRef cfig, OTOpenFlags oflag, OTNotifyUPP proc,
                                     void *contextPtr) {
  struct nslot *s;
  OSStatus err;
  if (!native)
    return __otsock_OTAsyncOpenInternetServices(cfig, oflag, proc, contextPtr);
  cfig = inet_config(cfig);
  s = new_slot(proc, contextPtr, NULL);
  if (!s)
    return kOTOutOfMemoryErr;
  err = (OSStatus)CCALL(__otn_AsyncOpenInternetServicesPriv, (long)cfig, (long)oflag,
                        (long)__otn_trampoline, (long)s, (long)&rec);
  if (err != kOTNoError)
    s->used = 0;
  return err;
}

/* Closes a provider. */
OSStatus OTCloseProvider(ProviderRef ref) {
  OSStatus err;
  if (!native)
    return __otsock_OTCloseProvider(ref);
  err = (OSStatus)CCALL(__otn_CloseProviderPriv, (long)ref, (long)&rec);
  free_slots(ref);
  return err;
}

/* Puts a provider in synchronous mode. */
OSStatus OTSetSynchronous(ProviderRef ref) {
  return native ? (OSStatus)PCALL1(__otn_SETSYNCHRONOUS, 4, ref) : __otsock_OTSetSynchronous(ref);
}

/* Puts a provider in asynchronous mode. */
OSStatus OTSetAsynchronous(ProviderRef ref) {
  return native ? (OSStatus)PCALL1(__otn_SETASYNCHRONOUS, 4, ref)
                : __otsock_OTSetAsynchronous(ref);
}

/* Gives true when a provider is in synchronous mode. */
Boolean OTIsSynchronous(ProviderRef ref) {
  return native ? PBOOL(PCALL1(__otn_ISSYNCHRONOUS, 2, ref)) : __otsock_OTIsSynchronous(ref);
}

/* Makes the calls of a provider wait. */
OSStatus OTSetBlocking(ProviderRef ref) {
  return native ? (OSStatus)PCALL1(__otn_SETBLOCKING, 4, ref) : __otsock_OTSetBlocking(ref);
}

/* Makes the calls of a provider give kOTNoDataErr or kOTFlowErr at once. */
OSStatus OTSetNonBlocking(ProviderRef ref) {
  return native ? (OSStatus)PCALL1(__otn_SETNONBLOCKING, 4, ref) : __otsock_OTSetNonBlocking(ref);
}

/* Gives true when the calls of a provider wait. */
Boolean OTIsBlocking(ProviderRef ref) {
  return native ? PBOOL(PCALL1(__otn_ISBLOCKING, 2, ref)) : __otsock_OTIsBlocking(ref);
}

/* Installs the notifier of a provider. */
OSStatus OTInstallNotifier(ProviderRef ref, OTNotifyUPP proc, void *contextPtr) {
  struct nslot *s;
  OSStatus err;
  if (!native)
    return __otsock_OTInstallNotifier(ref, proc, contextPtr);
  s = new_slot(proc, contextPtr, ref);
  if (!s)
    return kOTOutOfMemoryErr;
  err = (OSStatus)PCALL(__otn_INSTALLNOTIFIER, 4, 0, (long)ref, (long)__otn_trampoline, (long)s);
  if (err != kOTNoError)
    s->used = 0;
  return err;
}

/* Removes the notifier of a provider. */
void OTRemoveNotifier(ProviderRef ref) {
  if (!native) {
    __otsock_OTRemoveNotifier(ref);
    return;
  }
  PCALL1(__otn_REMOVENOTIFIER, 0, ref);
  free_slots(ref);
}

/* Asks for kOTSyncIdleEvent while a synchronous call waits. */
OSStatus OTUseSyncIdleEvents(ProviderRef ref, Boolean useEvents) {
  if (!native)
    return __otsock_OTUseSyncIdleEvents(ref, useEvents);
  return (OSStatus)PCALL(__otn_USESYNCIDLEEVENTS, 4, 2, (long)ref, (long)(useEvents ? 0x100 : 0));
}

/* Stops the notifier of a provider from running. */
Boolean OTEnterNotifier(ProviderRef ref) {
  return native ? PBOOL(PCALL1(__otn_ENTERNOTIFIER, 2, ref)) : __otsock_OTEnterNotifier(ref);
}

/* Lets the notifier of a provider run again. */
void OTLeaveNotifier(ProviderRef ref) {
  if (native)
    PCALL1(__otn_LEAVENOTIFIER, 0, ref);
  else
    __otsock_OTLeaveNotifier(ref);
}

/* Asks for T_MEMORYRELEASED after each send. */
OSStatus OTAckSends(ProviderRef ref) {
  return native ? (OSStatus)PCALL1(__otn_ACKSENDS, 4, ref) : __otsock_OTAckSends(ref);
}

/* Stops T_MEMORYRELEASED. */
OSStatus OTDontAckSends(ProviderRef ref) {
  return native ? (OSStatus)PCALL1(__otn_DONTACKSENDS, 4, ref) : __otsock_OTDontAckSends(ref);
}

/* Gives true after OTAckSends. */
Boolean OTIsAckingSends(ProviderRef ref) {
  return native ? PBOOL(PCALL1(__otn_ISACKINGSENDS, 2, ref)) : __otsock_OTIsAckingSends(ref);
}

/* Ends the synchronous calls that wait, with the error err. */
OSStatus OTCancelSynchronousCalls(ProviderRef ref, OSStatus err) {
  if (!native)
    return __otsock_OTCancelSynchronousCalls(ref, err);
  return (OSStatus)PCALL(__otn_CANCELSYNCHRONOUSCALLS, 4, 0, (long)ref, (long)err);
}

/* Sends a control call to a provider. */
SInt32 OTIoctl(ProviderRef ref, UInt32 cmd, void *data) {
  if (!native)
    return __otsock_OTIoctl(ref, cmd, data);
  return (SInt32)PCALL(__otn_IOCTL, 4, 0, (long)ref, (long)cmd, (long)data);
}

/* Gives the port of a provider. */
OTPortRef OTGetProviderPortRef(ProviderRef ref) {
  return native ? (OTPortRef)PCALL1(__otn_GETPROVIDERPORTREF, 4, ref)
                : __otsock_OTGetProviderPortRef(ref);
}

/* ------------------------------------------------------------------ */
/* Endpoints */

/* Gives the sizes and the kind of an endpoint. */
OSStatus OTGetEndpointInfo(EndpointRef ref, TEndpointInfo *info) {
  if (!native)
    return __otsock_OTGetEndpointInfo(ref, info);
  return (OSStatus)PCALL(__otn_GETENDPOINTINFO, 4, 0, (long)ref, (long)info);
}

/* Gives the state of an endpoint (T_UNBND, T_IDLE, ...). */
OTResult OTGetEndpointState(EndpointRef ref) {
  return native ? (OTResult)PCALL1(__otn_GETENDPOINTSTATE, 4, ref)
                : __otsock_OTGetEndpointState(ref);
}

/* Gives the event that waits on an endpoint. */
OTResult OTLook(EndpointRef ref) {
  return native ? (OTResult)PCALL1(__otn_LOOK, 4, ref) : __otsock_OTLook(ref);
}

/* Gives the bytes that wait to be received. */
OSStatus OTCountDataBytes(EndpointRef ref, OTByteCount *countPtr) {
  if (!native)
    return __otsock_OTCountDataBytes(ref, countPtr);
  return (OSStatus)PCALL(__otn_COUNTDATABYTES, 4, 0, (long)ref, (long)countPtr);
}

/* Gives the local and the remote address of an endpoint. */
OSStatus OTGetProtAddress(EndpointRef ref, TBind *boundAddr, TBind *peerAddr) {
  if (!native)
    return __otsock_OTGetProtAddress(ref, boundAddr, peerAddr);
  return (OSStatus)PCALL(__otn_GETPROTADDRESS, 4, 0, (long)ref, (long)boundAddr, (long)peerAddr);
}

/* Resolves an address with the endpoint's mapper. */
OSStatus OTResolveAddress(EndpointRef ref, TBind *reqAddr, TBind *retAddr, OTTimeout timeOut) {
  if (!native)
    return __otsock_OTResolveAddress(ref, reqAddr, retAddr, timeOut);
  return (OSStatus)PCALL(__otn_RESOLVEADDRESS, 4, 0, (long)ref, (long)reqAddr, (long)retAddr,
                         (long)timeOut);
}

/* Sets or gets the options of an endpoint. */
OSStatus OTOptionManagement(EndpointRef ref, TOptMgmt *req, TOptMgmt *ret) {
  if (!native)
    return __otsock_OTOptionManagement(ref, req, ret);
  return (OSStatus)PCALL(__otn_OPTIONMANAGEMENT, 4, 0, (long)ref, (long)req, (long)ret);
}

/* Gives an endpoint a local address. */
OSStatus OTBind(EndpointRef ref, TBind *reqAddr, TBind *retAddr) {
  if (!native)
    return __otsock_OTBind(ref, reqAddr, retAddr);
  return (OSStatus)PCALL(__otn_BIND, 4, 0, (long)ref, (long)reqAddr, (long)retAddr);
}

/* Takes the local address away. */
OSStatus OTUnbind(EndpointRef ref) {
  return native ? (OSStatus)PCALL1(__otn_UNBIND, 4, ref) : __otsock_OTUnbind(ref);
}

/* Connects to a remote address. */
OSStatus OTConnect(EndpointRef ref, TCall *sndCall, TCall *rcvCall) {
  if (!native)
    return __otsock_OTConnect(ref, sndCall, rcvCall);
  return (OSStatus)PCALL(__otn_CONNECT, 4, 0, (long)ref, (long)sndCall, (long)rcvCall);
}

/* Ends an asynchronous or nonblocking connect. */
OSStatus OTRcvConnect(EndpointRef ref, TCall *call) {
  if (!native)
    return __otsock_OTRcvConnect(ref, call);
  return (OSStatus)PCALL(__otn_RCVCONNECT, 4, 0, (long)ref, (long)call);
}

/* Takes a connection request. */
OSStatus OTListen(EndpointRef ref, TCall *call) {
  if (!native)
    return __otsock_OTListen(ref, call);
  return (OSStatus)PCALL(__otn_LISTEN, 4, 0, (long)ref, (long)call);
}

/* Accepts a connection request on the worker endpoint. */
OSStatus OTAccept(EndpointRef listener, EndpointRef worker, TCall *call) {
  if (!native)
    return __otsock_OTAccept(listener, worker, call);
  return (OSStatus)PCALL(__otn_ACCEPT, 4, 0, (long)listener, (long)worker, (long)call);
}

/* Ends a connection at once, or refuses a request. */
OSStatus OTSndDisconnect(EndpointRef ref, TCall *call) {
  if (!native)
    return __otsock_OTSndDisconnect(ref, call);
  return (OSStatus)PCALL(__otn_SNDDISCONNECT, 4, 0, (long)ref, (long)call);
}

/* Tells the peer that no more data comes. */
OSStatus OTSndOrderlyDisconnect(EndpointRef ref) {
  return native ? (OSStatus)PCALL1(__otn_SNDORDERLYDISCONNECT, 4, ref)
                : __otsock_OTSndOrderlyDisconnect(ref);
}

/* Takes the T_DISCONNECT event. */
OSStatus OTRcvDisconnect(EndpointRef ref, TDiscon *discon) {
  if (!native)
    return __otsock_OTRcvDisconnect(ref, discon);
  return (OSStatus)PCALL(__otn_RCVDISCONNECT, 4, 0, (long)ref, (long)discon);
}

/* Takes the T_ORDREL event. */
OSStatus OTRcvOrderlyDisconnect(EndpointRef ref) {
  return native ? (OSStatus)PCALL1(__otn_RCVORDERLYDISCONNECT, 4, ref)
                : __otsock_OTRcvOrderlyDisconnect(ref);
}

/* Receives data. */
OTResult OTRcv(EndpointRef ref, void *buf, OTByteCount nbytes, OTFlags *flags) {
  if (!native)
    return __otsock_OTRcv(ref, buf, nbytes, flags);
  return (OTResult)PCALL(__otn_RCV, 4, 0, (long)ref, (long)buf, (long)nbytes, (long)flags);
}

/* Sends data. */
OTResult OTSnd(EndpointRef ref, void *buf, OTByteCount nbytes, OTFlags flags) {
  if (!native)
    return __otsock_OTSnd(ref, buf, nbytes, flags);
  return (OTResult)PCALL(__otn_SND, 4, 0, (long)ref, (long)buf, (long)nbytes, (long)flags);
}

/* Sends a datagram. */
OSStatus OTSndUData(EndpointRef ref, TUnitData *udata) {
  if (!native)
    return __otsock_OTSndUData(ref, udata);
  return (OSStatus)PCALL(__otn_SNDUDATA, 4, 0, (long)ref, (long)udata);
}

/* Receives a datagram. */
OSStatus OTRcvUData(EndpointRef ref, TUnitData *udata, OTFlags *flags) {
  if (!native)
    return __otsock_OTRcvUData(ref, udata, flags);
  return (OSStatus)PCALL(__otn_RCVUDATA, 4, 0, (long)ref, (long)udata, (long)flags);
}

/* Takes the error of a datagram that was not sent. */
OSStatus OTRcvUDErr(EndpointRef ref, TUDErr *uderr) {
  if (!native)
    return __otsock_OTRcvUDErr(ref, uderr);
  return (OSStatus)PCALL(__otn_RCVUDERR, 4, 0, (long)ref, (long)uderr);
}

/* ------------------------------------------------------------------ */
/* Internet services */

/* Gives the address and other values of an interface. The native
   function finds Open Transport's configurator by name through ASLM,
   which fails for a program that uses this glue, so the MacTCP form
   answers in both modes (Open Transport also supplies MacTCP's .IPP
   driver). */
OSStatus OTInetGetInterfaceInfo(InetInterfaceInfo *info, SInt32 val) {
  return __otsock_OTInetGetInterfaceInfo(info, val);
}

/* Gives the secondary addresses of an interface (the MacTCP form, as
   for OTInetGetInterfaceInfo). */
OSStatus OTInetGetSecondaryAddresses(InetHost *addr, UInt32 *count, SInt32 val) {
  return __otsock_OTInetGetSecondaryAddresses(addr, count, val);
}

/* Finds the addresses of a host name or a dotted address. */
OSStatus OTInetStringToAddress(InetSvcRef ref, char *name, InetHostInfo *hinfo) {
  if (!native)
    return __otsock_OTInetStringToAddress(ref, name, hinfo);
  return (OSStatus)PCALL(__otn_INETSTRINGTOADDRESS, 4, 0, (long)ref, (long)name, (long)hinfo);
}

/* Finds the name of an address. */
OSStatus OTInetAddressToName(InetSvcRef ref, InetHost addr, char *name) {
  if (!native)
    return __otsock_OTInetAddressToName(ref, addr, name);
  return (OSStatus)PCALL(__otn_INETADDRESSTONAME, 4, 0, (long)ref, (long)addr, (long)name);
}

/* Gives the CPU and system of a host. */
OSStatus OTInetSysInfo(InetSvcRef ref, char *name, InetSysInfo *sysinfo) {
  if (!native)
    return __otsock_OTInetSysInfo(ref, name, sysinfo);
  return (OSStatus)PCALL(__otn_INETSYSINFO, 4, 0, (long)ref, (long)name, (long)sysinfo);
}

/* Gives the mail exchangers of a domain. */
OSStatus OTInetMailExchange(InetSvcRef ref, char *name, UInt16 *num, InetMailExchange *mx) {
  if (!native)
    return __otsock_OTInetMailExchange(ref, name, num, mx);
  return (OSStatus)PCALL(__otn_INETMAILEXCHANGE, 4, 0, (long)ref, (long)name, (long)num,
                         (long)mx);
}

/* Sends a query to the name server. */
OSStatus OTInetQuery(InetSvcRef ref, char *name, UInt16 qClass, UInt16 qType, char *buf,
                     OTByteCount buflen, void **argv, OTByteCount argvlen, OTFlags requestFlags) {
  if (!native)
    return __otsock_OTInetQuery(ref, name, qClass, qType, buf, buflen, argv, argvlen,
                                requestFlags);
  /* qClass and qType are 2-byte arguments (bits 2 and 3). */
  return (OSStatus)PCALL(__otn_INETQUERY, 4, 0x0C, (long)ref, (long)name, (long)qClass,
                         (long)qType, (long)buf, (long)buflen, (long)argv, (long)argvlen,
                         (long)requestFlags);
}
#endif
