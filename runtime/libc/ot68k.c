/* Open Transport on 68k: the client calls over MacTCP.
 *
 * Apple's 68k Open Transport libraries use the Apple Shared Library
 * Manager, which this SDK does not have. So on 68k this file supplies
 * the Open Transport calls itself, over the BSD sockets of socket.c
 * (MacTCP's .IPP driver) and the name resolver of dnr.c. A program
 * that uses Open Transport runs with MacTCP (System 7) and with Open
 * Transport (which also supplies .IPP).
 *
 * What it supplies:
 *   - "tcp" and "udp" endpoints: bind, listen and accept, connect,
 *     send and receive, orderly and abortive disconnect, datagrams;
 *   - synchronous and asynchronous mode, blocking and nonblocking mode,
 *     notifiers, completion events and the events of OTLook;
 *   - the Internet services: address text, the interface, and the
 *     name resolver;
 *   - the utilities: memory, strings, lists, time, atomic operations,
 *     deferred and system tasks.
 *
 * A notifier runs when the program calls OTIdle, or while a synchronous
 * blocking call waits. It never runs at interrupt time. A program that
 * uses asynchronous mode calls OTIdle in its event loop.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#if defined(__m68k__)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <AddressXlation.h>
#include <Events.h>
#include <Multiverse.h>
#include <OpenTransport.h>
#include <OpenTransportProviders.h>
#include "macos.h"
#include "socket.h"

#ifndef FIONREAD
#define FIONREAD 0x541B
#endif

#define EP_MAGIC 0x4F54457AUL  /* 'OTEz': a live endpoint */
#define MAX_EVENTS 8           /* completion events that wait */
#define MAX_INDS 4             /* connection indications that wait */

/* The kinds of provider. */
enum { K_TCP = 1, K_UDP, K_INET };

/* A configuration: the kind of provider that OTOpenEndpoint makes. */
struct config {
  int kind;
};

/* A completion event that waits for the notifier. */
struct event {
  OTEventCode code;
  OTResult result;
  void *cookie;
};

/* A connection that OTListen took and OTAccept has not. */
struct ind {
  int fd;
  struct sockaddr_in from;
  OTSequence seq;
};

/* An endpoint, or the Internet services provider. */
struct ep {
  unsigned long magic;
  struct ep *next;
  int kind;
  int fd;                  /* the socket, or -1 */
  OTResult state;          /* T_UNBND, T_IDLE, ... */
  Boolean sync, blocking, acking, idle_events;
  OTNotifyUPP proc;
  void *context;
  int in_notifier;
  OTQLen qlen;
  struct ind inds[MAX_INDS];
  int ninds;
  OTSequence next_seq;
  int connecting;          /* a nonblocking connect is in progress */
  int notified;            /* the level events that the notifier has had */
  int peer_closed;         /* T_ORDREL came */
  struct event events[MAX_EVENTS];
  int nevents;
};

static struct ep *eps;     /* the open providers */
static int started;        /* InitOpenTransport has run */

/* The bits of ep->notified. */
enum { N_DATA = 1, N_LISTEN = 2, N_CONNECT = 4, N_ORDREL = 8, N_DISCON = 16 };

/* ------------------------------------------------------------------ */
/* Helpers */

/* Masks the interrupts and gives the old status register. Mac OS runs
   programs in supervisor mode, so the status register is theirs to
   change. */
static inline short __mac_disable(void) {
  register short sr __asm__("d0");
  /* move.w %sr,%d0; ori.w #0x0700,%sr (the assembler has no names for
     these forms) */
  __asm__ volatile(".short 0x40C0, 0x007C, 0x0700" : "=r"(sr) : : "memory");
  return sr;
}

/* Puts back the status register that __mac_disable gave. */
static inline void __mac_restore(short old) {
  register short sr __asm__("d0") = old;
  __asm__ volatile(".short 0x46C0" : : "r"(sr) : "memory"); /* move.w %d0,%sr */
}

/* Gives the endpoint of a reference, or NULL when it is not open. */
static struct ep *ep_of(ProviderRef ref) {
  struct ep *e = (struct ep *)ref;
  struct ep *p;
  for (p = eps; p; p = p->next)
    if (p == e && e->magic == EP_MAGIC)
      return e;
  return NULL;
}

/* Gives the Open Transport error for an errno value. */
static OSStatus ot_error(int e) {
  switch (e) {
  case 0: return kOTNoError;
  case EAGAIN: return kOTNoDataErr;
  case EADDRINUSE: return kOTAddressBusyErr;
  case EADDRNOTAVAIL: return kEADDRNOTAVAILErr;
  case ECONNREFUSED: return kECONNREFUSEDErr;
  case ECONNRESET: return kECONNRESETErr;
  case ECONNABORTED: return kECONNABORTEDErr;
  case ETIMEDOUT: return kETIMEDOUTErr;
  case ENOTCONN: return kOTOutStateErr;
  case EISCONN: return kEISCONNErr;
  case EALREADY: return kEALREADYErr;
  case ENOMEM: return kENOMEMErr;
  case ENOBUFS: return kENOBUFSErr;
  case EINVAL: return kEINVALErr;
  case EMSGSIZE: return kEMSGSIZEErr;
  case ENETDOWN: return kENETDOWNErr;
  case ENETUNREACH: return kENETUNREACHErr;
  case EHOSTUNREACH: return kEHOSTUNREACHErr;
  case EPIPE: return kEPIPEErr;
  case EBADF: return kOTBadReferenceErr;
  default: return kOTSysErrorErr;
  }
}

/* Gives the result of the name resolver as an Open Transport error. */
static OSStatus dnr_error(long rc) {
  switch (rc) {
  case noErr: return kOTNoError;
  case nameSyntaxErr:
  case authNameErr: return kOTBadNameErr;
  case noNameServer:
  case noAnsErr: return kETIMEDOUTErr;
  case outOfMemory: return kENOMEMErr;
  default: return kOTSysErrorErr;
  }
}

/* Queues a completion event for the notifier of e. */
static void post(struct ep *e, OTEventCode code, OTResult result, void *cookie) {
  if (!e->proc || e->nevents >= MAX_EVENTS)
    return;
  e->events[e->nevents].code = code;
  e->events[e->nevents].result = result;
  e->events[e->nevents].cookie = cookie;
  e->nevents++;
}

/* Calls the notifier of e with one event. */
static void notify(struct ep *e, OTEventCode code, OTResult result, void *cookie) {
  e->in_notifier++;
  e->proc(e->context, code, result, cookie);
  e->in_notifier--;
}

/* Gives the socket events of e that are true now. */
static short ready(struct ep *e) {
  struct pollfd p;
  if (e->fd < 0)
    return 0;
  p.fd = e->fd;
  p.events = POLLIN | POLLOUT;
  p.revents = 0;
  if (poll(&p, 1, 0) < 0)
    return 0;
  return p.revents;
}

/* Tells if a nonblocking connect of e has ended, and ends it. Gives 1
   when it has (e->state is then T_DATAXFER or T_IDLE), else 0. */
static int connect_ended(struct ep *e) {
  int err = 0;
  socklen_t len = sizeof err;
  if (!e->connecting || !(ready(e) & (POLLOUT | POLLERR)))
    return 0;
  getsockopt(e->fd, SOL_SOCKET, SO_ERROR, &err, &len);
  e->connecting = 0;
  e->state = err ? T_IDLE : T_DATAXFER;
  return 1;
}

/* Gives the event that waits on e now (T_LISTEN, T_CONNECT, T_DATA,
   T_ORDREL, T_DISCONNECT), or 0. */
static OTResult look(struct ep *e) {
  short r;
  if (e->kind == K_INET || e->fd < 0)
    return 0;
  if (e->kind == K_UDP)
    return (ready(e) & POLLIN) ? T_DATA : 0;
  if (e->ninds)
    return T_LISTEN;
  if (e->state == T_OUTCON) {
    if (e->connecting) {
      if (!(ready(e) & (POLLOUT | POLLERR)))
        return 0;
      {
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt(e->fd, SOL_SOCKET, SO_ERROR, &err, &len);
        return err ? T_DISCONNECT : T_CONNECT;
      }
    }
    return T_CONNECT;
  }
  if (e->qlen > 0 && e->state == T_IDLE)
    return (ready(e) & POLLIN) ? T_LISTEN : 0;
  if (e->state != T_DATAXFER && e->state != T_OUTREL)
    return 0;
  r = ready(e);
  if (r & POLLIN) {
    char c;
    long n = recv(e->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0)
      return T_DATA;
    if (n == 0)
      return T_ORDREL;
    if (errno != EAGAIN)
      return T_DISCONNECT;
  } else if (r & (POLLHUP | POLLERR)) {
    return T_DISCONNECT;
  }
  return 0;
}

/* Gives the level events of e to its notifier once each. */
static void notify_levels(struct ep *e) {
  OTResult ev = look(e);
  int bit;
  switch (ev) {
  case T_DATA: bit = N_DATA; break;
  case T_LISTEN: bit = N_LISTEN; break;
  case T_CONNECT: bit = N_CONNECT; break;
  case T_ORDREL: bit = N_ORDREL; break;
  case T_DISCONNECT: bit = N_DISCON; break;
  default: return;
  }
  if (e->notified & bit)
    return;
  e->notified |= bit;
  notify(e, ev, kOTNoError, NULL);
}

static void run_tasks(void);

/* Runs the deferred and system tasks that are scheduled, then the
   notifiers: the completion events that wait, then the events of the
   connections. A notifier does not run inside itself. */
static void pump(void) {
  struct ep *e, *next;
  run_tasks();
  for (e = eps; e; e = next) {
    next = e->next;
    if (!e->proc || e->in_notifier)
      continue;
    while (e->nevents && e->magic == EP_MAGIC) {
      struct event ev = e->events[0];
      memmove(&e->events[0], &e->events[1], (size_t)--e->nevents * sizeof ev);
      notify(e, ev.code, ev.result, ev.cookie);
    }
    if (e->magic == EP_MAGIC && e->proc && !e->sync)
      notify_levels(e);
  }
}

/* Waits a moment in a synchronous blocking call of e: lets the other
   programs run, gives e's notifier kOTSyncIdleEvent when it asked for
   it, and runs the notifiers. */
static void idle(struct ep *e) {
  if (e->idle_events && e->proc && !e->in_notifier)
    notify(e, kOTSyncIdleEvent, kOTNoError, NULL);
  pump();
  __mac_wait();
}

/* Ends a call of e: in asynchronous mode, queues the completion event
   code with the result, and gives kOTNoError. Gives the result
   otherwise. */
static OSStatus complete(struct ep *e, OTEventCode code, OSStatus result, void *cookie) {
  if (e->sync || !e->proc)
    return result;
  post(e, code, result, cookie);
  return kOTNoError;
}

/* Copies an InetAddress into a TNetbuf with room for it. Sets len. */
static void put_addr(TNetbuf *nb, const struct sockaddr_in *sin) {
  InetAddress a;
  if (!nb || !nb->buf || nb->maxlen < sizeof a) {
    if (nb)
      nb->len = 0;
    return;
  }
  OTInitInetAddress(&a, ntohs(sin->sin_port), ntohl(sin->sin_addr.s_addr));
  memcpy(nb->buf, &a, sizeof a);
  nb->len = sizeof a;
}

/* Reads an InetAddress from a TNetbuf into a socket address. Gives 0, or
   -1 when the TNetbuf does not hold one. */
static int get_addr(const TNetbuf *nb, struct sockaddr_in *sin) {
  InetAddress a;
  if (!nb || !nb->buf || nb->len < sizeof a)
    return -1;
  memcpy(&a, nb->buf, sizeof a);
  if (a.fAddressType != AF_INET)
    return -1;
  memset(sin, 0, sizeof *sin);
  sin->sin_family = AF_INET;
  sin->sin_port = htons(a.fPort);
  sin->sin_addr.s_addr = htonl(a.fHost);
  return 0;
}

/* Sets the socket of e to nonblocking mode: the blocking calls wait in
   this file, so that the notifiers can run. */
static void sock_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

/* Makes the socket of e when it has none. Gives 0, or -1 with errno. */
static int need_socket(struct ep *e) {
  if (e->fd >= 0)
    return 0;
  e->fd = socket(AF_INET, e->kind == K_TCP ? SOCK_STREAM : SOCK_DGRAM, 0);
  if (e->fd < 0)
    return -1;
  sock_nonblock(e->fd);
  return 0;
}

/* Fills an endpoint information record for a kind of provider. */
static void fill_info(int kind, TEndpointInfo *info) {
  if (!info)
    return;
  memset(info, 0, sizeof *info);
  info->addr = sizeof(InetAddress);
  info->options = 256;
  info->etsdu = T_INVALID;
  info->connect = T_INVALID;
  info->discon = T_INVALID;
  if (kind == K_TCP) {
    info->tsdu = 0;
    info->servtype = T_COTS_ORD;
  } else {
    info->tsdu = 65507;
    info->servtype = T_CLTS;
  }
}

/* ------------------------------------------------------------------ */
/* Starting and ending */

/* Closes the providers that are still open when the program ends. */
static void close_all(void) {
  while (eps)
    OTCloseProvider((ProviderRef)eps);
}

/* Starts Open Transport for the program. Gives kOTNotFoundErr when
   MacTCP (the .IPP driver) is not there. */
OSStatus InitOpenTransport(void) {
  if (!__mac_ip_address())
    return kOTNotFoundErr;
  if (!started) {
    started = 1;
    atexit(close_all);
  }
  return kOTNoError;
}

/* Starts the utility calls only. They need nothing, so it always
   succeeds. */
OSStatus InitOpenTransportUtilities(void) { return kOTNoError; }

/* Ends Open Transport for the program: closes its providers. */
void CloseOpenTransport(void) { close_all(); }

/* Registers the program as a client. There are no client events here,
   so it only succeeds. */
OSStatus OTRegisterAsClient(OTClientName name, OTNotifyUPP proc) {
  (void)name;
  (void)proc;
  return kOTNoError;
}

/* Ends the registration of OTRegisterAsClient. */
OSStatus OTUnregisterAsClient(void) { return kOTNoError; }

/* ------------------------------------------------------------------ */
/* Memory */

/* Allocates memory that OTFreeMem frees. Gives NULL when there is none. */
void *OTAllocMem(OTByteCount size) { return NewPtr((Size)size); }

/* Frees memory from OTAllocMem or OTAlloc. */
void OTFreeMem(void *mem) {
  if (mem)
    DisposePtr((Ptr)mem);
}

/* The TNetbuf fields of each structure type, by offset. */
static const unsigned char netbufs[][3] = {
    /* T_BIND */ {offsetof(TBind, addr), 0xFF, 0xFF},
    /* T_OPTMGMT */ {offsetof(TOptMgmt, opt), 0xFF, 0xFF},
    /* T_CALL */ {offsetof(TCall, addr), offsetof(TCall, opt), offsetof(TCall, udata)},
    /* T_DIS */ {offsetof(TDiscon, udata), 0xFF, 0xFF},
    /* T_UNITDATA */ {offsetof(TUnitData, addr), offsetof(TUnitData, opt), offsetof(TUnitData, udata)},
    /* T_UDERROR */ {offsetof(TUDErr, addr), offsetof(TUDErr, opt), 0xFF},
    /* T_INFO */ {0xFF, 0xFF, 0xFF},
};

/* Gives the size of a structure type, or 0. */
static size_t struct_size(OTStructType t) {
  switch (t) {
  case T_BIND: return sizeof(TBind);
  case T_OPTMGMT: return sizeof(TOptMgmt);
  case T_CALL: return sizeof(TCall);
  case T_DIS: return sizeof(TDiscon);
  case T_UNITDATA: return sizeof(TUnitData);
  case T_UDERROR: return sizeof(TUDErr);
  case T_INFO: return sizeof(TEndpointInfo);
  default: return 0;
  }
}

/* Allocates a structure of a type (T_BIND, T_CALL, ...) for ref, with
   buffers for the fields in "fields" (T_ADDR, T_OPT, T_UDATA, T_ALL). */
void *OTAlloc(EndpointRef ref, OTStructType structType, OTFieldsType fields, OSStatus *err) {
  size_t size = struct_size(structType);
  static const OTFieldsType masks[3] = {T_ADDR, T_OPT, T_UDATA};
  static const OTByteCount sizes[3] = {sizeof(InetAddress), 256, 8192};
  char *s;
  int i;
  (void)ref;
  if (!size) {
    if (err)
      *err = kOTNoStructureTypeErr;
    return NULL;
  }
  s = NewPtrClear((Size)size);
  if (!s) {
    if (err)
      *err = kENOMEMErr;
    return NULL;
  }
  for (i = 0; i < 3; i++) {
    unsigned char off = netbufs[structType - 1][i];
    TNetbuf *nb;
    if (off == 0xFF || !(fields & masks[i]))
      continue;
    nb = (TNetbuf *)(s + off);
    nb->buf = (UInt8 *)NewPtrClear((Size)sizes[i]);
    if (nb->buf)
      nb->maxlen = sizes[i];
  }
  if (err)
    *err = kOTNoError;
  return s;
}

/* Frees a structure from OTAlloc and its buffers. */
OTResult OTFree(void *ptr, OTStructType structType) {
  int i;
  if (!ptr)
    return kOTNoError;
  if (!struct_size(structType))
    return kOTNoStructureTypeErr;
  for (i = 0; i < 3; i++) {
    unsigned char off = netbufs[structType - 1][i];
    if (off != 0xFF) {
      TNetbuf *nb = (TNetbuf *)((char *)ptr + off);
      if (nb->buf)
        DisposePtr((Ptr)nb->buf);
    }
  }
  DisposePtr((Ptr)ptr);
  return kOTNoError;
}

/* Copies memory. */
void OTMemcpy(void *dest, const void *src, OTByteCount nBytes) { memcpy(dest, src, nBytes); }

/* Tells if two blocks of memory are equal. */
Boolean OTMemcmp(const void *mem1, const void *mem2, OTByteCount nBytes) {
  return memcmp(mem1, mem2, nBytes) == 0;
}

/* Copies memory; the blocks can overlap. */
void OTMemmove(void *dest, const void *src, OTByteCount nBytes) { memmove(dest, src, nBytes); }

/* Sets memory to zero. */
void OTMemzero(void *dest, OTByteCount nBytes) { memset(dest, 0, nBytes); }

/* Sets memory to a byte value. */
void OTMemset(void *dest, OTUInt8Param toSet, OTByteCount nBytes) { memset(dest, toSet, nBytes); }

/* Gives the length of a C string. */
OTByteCount OTStrLength(const char *str) { return strlen(str); }

/* Copies a C string. */
void OTStrCopy(char *dest, const char *src) { strcpy(dest, src); }

/* Adds a C string to the end of another. */
void OTStrCat(char *dest, const char *src) { strcat(dest, src); }

/* Tells if two C strings are equal. */
Boolean OTStrEqual(const char *src1, const char *src2) { return strcmp(src1, src2) == 0; }

/* ------------------------------------------------------------------ */
/* Time */

/* Gives the time now, in microseconds since startup. */
void OTGetTimeStamp(OTTimeStamp *currentTime) { Microseconds(currentTime); }

/* Gives endEnd minus startTime in *result, and result. */
OTTimeStamp *OTSubtractTimeStamps(OTTimeStamp *result, OTTimeStamp *startTime, OTTimeStamp *endEnd) {
  UInt32 lo = endEnd->lo - startTime->lo;
  UInt32 hi = endEnd->hi - startTime->hi - (endEnd->lo < startTime->lo ? 1 : 0);
  result->lo = lo;
  result->hi = hi;
  return result;
}

/* Gives a time difference in milliseconds. */
UInt32 OTTimeStampInMilliseconds(OTTimeStamp *delta) {
  return (UInt32)((((unsigned long long)delta->hi << 32) | delta->lo) / 1000);
}

/* Gives a time difference in microseconds (the low 32 bits). */
UInt32 OTTimeStampInMicroseconds(OTTimeStamp *delta) { return delta->lo; }

/* Gives the milliseconds since a time stamp. */
UInt32 OTElapsedMilliseconds(OTTimeStamp *startTime) {
  OTTimeStamp now, d;
  OTGetTimeStamp(&now);
  return OTTimeStampInMilliseconds(OTSubtractTimeStamps(&d, startTime, &now));
}

/* Gives the microseconds since a time stamp. */
UInt32 OTElapsedMicroseconds(OTTimeStamp *startTime) {
  OTTimeStamp now, d;
  OTGetTimeStamp(&now);
  return OTTimeStampInMicroseconds(OTSubtractTimeStamps(&d, startTime, &now));
}

/* Gives the seconds since startup. */
UInt32 OTGetClockTimeInSecs(void) { return TickCount() / 60; }

/* Waits some seconds and lets the other programs run. */
OSStatus OTDelay(UInt32 seconds) {
  unsigned long until = TickCount() + 60UL * seconds;
  while ((long)(until - TickCount()) > 0) {
    pump();
    __mac_wait();
  }
  return kOTNoError;
}

/* Lets the notifiers run (the events of the asynchronous endpoints)
   and lets the other programs run. Call it in the event loop. */
void OTIdle(void) { pump(); }

/* ------------------------------------------------------------------ */
/* Lists */

/* Adds a link to the front of a LIFO list. Interrupts cannot see the
   list half changed. */
void OTLIFOEnqueue(OTLIFO *list, OTLink *link) {
  short sr = __mac_disable();
  link->fNext = list->fHead;
  list->fHead = link;
  __mac_restore(sr);
}

/* Takes the first link of a LIFO list, or NULL. */
OTLink *OTLIFODequeue(OTLIFO *list) {
  short sr = __mac_disable();
  OTLink *l = list->fHead;
  if (l)
    list->fHead = l->fNext;
  __mac_restore(sr);
  return l;
}

/* Takes all the links of a LIFO list. */
OTLink *OTLIFOStealList(OTLIFO *list) {
  short sr = __mac_disable();
  OTLink *l = list->fHead;
  list->fHead = NULL;
  __mac_restore(sr);
  return l;
}

/* Reverses a chain of links, and gives its new first link. */
OTLink *OTReverseList(OTLink *list) {
  OTLink *out = NULL;
  while (list) {
    OTLink *n = list->fNext;
    list->fNext = out;
    out = list;
    list = n;
  }
  return out;
}

/* Adds a link to the front of a list. */
void OTAddFirst(OTList *list, OTLink *link) {
  link->fNext = list->fHead;
  list->fHead = link;
}

/* Adds a link to the end of a list. */
void OTAddLast(OTList *list, OTLink *link) {
  OTLink **p = &list->fHead;
  while (*p)
    p = &(*p)->fNext;
  link->fNext = NULL;
  *p = link;
}

/* Takes the first link of a list, or NULL. */
OTLink *OTRemoveFirst(OTList *list) {
  OTLink *l = list->fHead;
  if (l)
    list->fHead = l->fNext;
  return l;
}

/* Takes the last link of a list, or NULL. */
OTLink *OTRemoveLast(OTList *list) {
  OTLink **p = &list->fHead, *l;
  if (!*p)
    return NULL;
  while ((*p)->fNext)
    p = &(*p)->fNext;
  l = *p;
  *p = NULL;
  return l;
}

/* Gives the first link of a list, or NULL. */
OTLink *OTGetFirst(OTList *list) { return list->fHead; }

/* Gives the last link of a list, or NULL. */
OTLink *OTGetLast(OTList *list) {
  OTLink *l = list->fHead;
  while (l && l->fNext)
    l = l->fNext;
  return l;
}

/* Tells if a link is in a list. */
Boolean OTIsInList(OTList *list, OTLink *link) {
  OTLink *l;
  for (l = list->fHead; l; l = l->fNext)
    if (l == link)
      return true;
  return false;
}

/* Takes a link out of a list. Tells if it was there. */
Boolean OTRemoveLink(OTList *list, OTLink *link) {
  OTLink **p;
  for (p = &list->fHead; *p; p = &(*p)->fNext)
    if (*p == link) {
      *p = link->fNext;
      return true;
    }
  return false;
}

/* Gives the link at an index (0 is the first) of a list, or NULL. */
OTLink *OTGetIndexedLink(OTList *list, OTItemCount index) {
  OTLink *l = list->fHead;
  while (l && index--)
    l = l->fNext;
  return l;
}

/* ------------------------------------------------------------------ */
/* Atomic operations: the 68k has no multiprocessor, so masking the
   interrupts makes them atomic. */

/* Sets a bit (bit 0 is the low bit of the byte) and gives its old value. */
Boolean OTAtomicSetBit(UInt8 *bytePtr, OTByteCount bitNumber) {
  short sr = __mac_disable();
  Boolean old = (*bytePtr >> bitNumber) & 1;
  *bytePtr |= (UInt8)(1 << bitNumber);
  __mac_restore(sr);
  return old;
}

/* Clears a bit and gives its old value. */
Boolean OTAtomicClearBit(UInt8 *bytePtr, OTByteCount bitNumber) {
  short sr = __mac_disable();
  Boolean old = (*bytePtr >> bitNumber) & 1;
  *bytePtr &= (UInt8) ~(1 << bitNumber);
  __mac_restore(sr);
  return old;
}

/* Gives the value of a bit. */
Boolean OTAtomicTestBit(UInt8 *bytePtr, OTByteCount bitNumber) { return (*bytePtr >> bitNumber) & 1; }

/* Sets *dest to newValue when it is oldValue. Tells if it did. */
Boolean OTCompareAndSwapPtr(void *oldValue, void *newValue, void **dest) {
  short sr = __mac_disable();
  Boolean ok = *dest == oldValue;
  if (ok)
    *dest = newValue;
  __mac_restore(sr);
  return ok;
}

/* Sets *dest to newValue when it is oldValue. Tells if it did. */
Boolean OTCompareAndSwap32(UInt32 oldValue, UInt32 newValue, UInt32 *dest) {
  short sr = __mac_disable();
  Boolean ok = *dest == oldValue;
  if (ok)
    *dest = newValue;
  __mac_restore(sr);
  return ok;
}

/* Adds toAdd to *dest and gives the new value. */
SInt32 OTAtomicAdd32(SInt32 toAdd, SInt32 *dest) {
  short sr = __mac_disable();
  SInt32 v = *dest += toAdd;
  __mac_restore(sr);
  return v;
}

/* ------------------------------------------------------------------ */
/* Deferred and system tasks: both run at OTIdle time. */

/* A task of OTCreateDeferredTask or OTCreateSystemTask. */
struct task {
  struct task *next;
  OTProcessUPP proc;
  void *arg;
  volatile int scheduled;
};

static struct task *tasks;

/* Runs the tasks that are scheduled. */
static void run_tasks(void) {
  struct task *t;
  for (t = tasks; t; t = t->next)
    if (t->scheduled) {
      t->scheduled = 0;
      t->proc(t->arg);
    }
}

/* Makes a task. Gives its reference, or 0. */
static long new_task(OTProcessUPP proc, void *arg) {
  struct task *t = (struct task *)NewPtrClear(sizeof *t);
  if (!t)
    return 0;
  t->proc = proc;
  t->arg = arg;
  t->next = tasks;
  tasks = t;
  return (long)t;
}

/* Frees a task. */
static OSStatus free_task(long ref) {
  struct task **p;
  for (p = &tasks; *p; p = &(*p)->next)
    if ((long)*p == ref) {
      struct task *t = *p;
      *p = t->next;
      DisposePtr((Ptr)t);
      return kOTNoError;
    }
  return kOTBadReferenceErr;
}

/* Makes a deferred task. It runs at the next OTIdle after
   OTScheduleDeferredTask. */
long OTCreateDeferredTask(OTProcessUPP proc, void *contextInfo) { return new_task(proc, contextInfo); }

/* Makes a system task. It runs at the next OTIdle after
   OTScheduleSystemTask. */
long OTCreateSystemTask(OTProcessUPP proc, void *contextInfo) { return new_task(proc, contextInfo); }

/* Schedules a deferred task. Interrupt routines can call it. Tells if
   the task was not scheduled before. */
Boolean OTScheduleDeferredTask(long dtCookie) {
  struct task *t = (struct task *)dtCookie;
  Boolean was = t->scheduled;
  t->scheduled = 1;
  return !was;
}

/* Frees a deferred task. */
OSStatus OTDestroyDeferredTask(long dtCookie) { return free_task(dtCookie); }

/* Schedules a system task. Tells if it was not scheduled before. */
Boolean OTScheduleSystemTask(long stCookie) { return OTScheduleDeferredTask(stCookie); }

/* Cancels a system task that is scheduled. Tells if it was. */
Boolean OTCancelSystemTask(long stCookie) {
  struct task *t = (struct task *)stCookie;
  Boolean was = t->scheduled;
  t->scheduled = 0;
  return was;
}

/* Frees a system task. */
OSStatus OTDestroySystemTask(long stCookie) { return free_task(stCookie); }

/* Tells if a synchronous call can be made now: not at interrupt time
   and not in a notifier. */
Boolean OTCanMakeSyncCall(void) {
  struct ep *e;
  for (e = eps; e; e = e->next)
    if (e->in_notifier)
      return false;
  return true;
}

/* Tells if the program runs at interrupt time. Code here runs only at
   program time. */
Boolean OTIsAtInterruptLevel(void) { return false; }

/* ------------------------------------------------------------------ */
/* Configurations */

/* Makes a configuration from a configuration string: the kind of
   provider is "tcp", "udp" or "dnr" (the last name in the string, as
   in "tilisten,tcp"). Gives NULL when the name is not known. */
OTConfigurationRef OTCreateConfiguration(const char *path) {
  struct config *c;
  const char *p, *name = path;
  int kind = 0;
  if (!path)
    return NULL;
  for (p = path; *p; p++)
    if (*p == ',')
      name = p + 1;
  if (!strncmp(name, kTCPName, 3))
    kind = K_TCP;
  else if (!strncmp(name, kUDPName, 3))
    kind = K_UDP;
  else if (!strncmp(name, kDNRName, 3))
    kind = K_INET;
  if (!kind)
    return (OTConfigurationRef)(long)-1; /* kOTInvalidConfigurationPtr */
  c = (struct config *)NewPtr(sizeof *c);
  if (!c)
    return NULL;
  c->kind = kind;
  return (OTConfigurationRef)c;
}

/* Gives the kind of provider of a configuration, or 0. */
static int config_kind(OTConfigurationRef cfig) {
  if (cfig == kDefaultInternetServicesPath)
    return K_INET;
  if (!cfig || cfig == (OTConfigurationRef)(long)-1)
    return 0;
  return ((struct config *)cfig)->kind;
}

/* Makes a copy of a configuration. */
OTConfigurationRef OTCloneConfiguration(OTConfigurationRef cfig) {
  struct config *c;
  int kind = config_kind(cfig);
  if (!kind || cfig == kDefaultInternetServicesPath)
    return cfig;
  c = (struct config *)NewPtr(sizeof *c);
  if (c)
    c->kind = kind;
  return (OTConfigurationRef)c;
}

/* Frees a configuration that was not given to an open call. */
void OTDestroyConfiguration(OTConfigurationRef cfig) {
  if (cfig && cfig != (OTConfigurationRef)(long)-1 && cfig != kDefaultInternetServicesPath)
    DisposePtr((Ptr)cfig);
}

/* ------------------------------------------------------------------ */
/* Providers */

/* Makes a provider of a configuration, and frees the configuration.
   Gives NULL with *err set on an error. */
static struct ep *new_ep(OTConfigurationRef cfig, OSStatus *err) {
  int kind = config_kind(cfig);
  struct ep *e;
  OTDestroyConfiguration(cfig);
  if (!kind) {
    *err = kOTBadConfigurationErr;
    return NULL;
  }
  if (!started && InitOpenTransport() != kOTNoError) {
    *err = kOTNotFoundErr;
    return NULL;
  }
  e = (struct ep *)NewPtrClear(sizeof *e);
  if (!e) {
    *err = kENOMEMErr;
    return NULL;
  }
  e->magic = EP_MAGIC;
  e->kind = kind;
  e->fd = -1;
  e->state = kind == K_INET ? T_IDLE : T_UNBND;
  e->sync = true;
  e->acking = false;
  e->next = eps;
  eps = e;
  *err = kOTNoError;
  return e;
}

/* Opens an endpoint ("tcp" or "udp") in synchronous nonblocking mode. */
EndpointRef OTOpenEndpoint(OTConfigurationRef config, OTOpenFlags oflag, TEndpointInfo *info,
                           OSStatus *err) {
  OSStatus e2;
  struct ep *e;
  (void)oflag;
  if (config_kind(config) == K_INET) {
    OTDestroyConfiguration(config);
    if (err)
      *err = kOTBadConfigurationErr;
    return NULL;
  }
  e = new_ep(config, &e2);
  if (err)
    *err = e2;
  if (e)
    fill_info(e->kind, info);
  return (EndpointRef)e;
}

/* Opens an endpoint in asynchronous mode. The notifier gets
   T_OPENCOMPLETE with the endpoint as the cookie at the next OTIdle. */
OSStatus OTAsyncOpenEndpoint(OTConfigurationRef config, OTOpenFlags oflag, TEndpointInfo *info,
                             OTNotifyUPP proc, void *contextPtr) {
  OSStatus err;
  struct ep *e = (struct ep *)OTOpenEndpoint(config, oflag, info, &err);
  if (!e)
    return err;
  e->proc = proc;
  e->context = contextPtr;
  e->sync = false;
  post(e, T_OPENCOMPLETE, kOTNoError, e);
  return kOTNoError;
}

/* Opens the Internet services provider. The name resolver opens at the
   first name to find, so that addresses as text work without it. */
InetSvcRef OTOpenInternetServices(OTConfigurationRef cfig, OTOpenFlags oflag, OSStatus *err) {
  OSStatus e2;
  struct ep *e;
  (void)oflag;
  if (config_kind(cfig) != K_INET) {
    OTDestroyConfiguration(cfig);
    if (err)
      *err = kOTBadConfigurationErr;
    return NULL;
  }
  e = new_ep(cfig, &e2);
  if (err)
    *err = e2;
  return (InetSvcRef)e;
}

/* Opens the Internet services provider in asynchronous mode. The
   notifier gets T_OPENCOMPLETE with the provider as the cookie. */
OSStatus OTAsyncOpenInternetServices(OTConfigurationRef cfig, OTOpenFlags oflag, OTNotifyUPP proc,
                                     void *contextPtr) {
  OSStatus err;
  struct ep *e = (struct ep *)OTOpenInternetServices(cfig, oflag, &err);
  if (!e)
    return err;
  e->proc = proc;
  e->context = contextPtr;
  e->sync = false;
  post(e, T_OPENCOMPLETE, kOTNoError, e);
  return kOTNoError;
}

/* Closes the waiting connections of a listener. */
static void drop_inds(struct ep *e) {
  while (e->ninds)
    close(e->inds[--e->ninds].fd);
}

/* Closes a provider: aborts its connection and frees it. */
OSStatus OTCloseProvider(ProviderRef ref) {
  struct ep *e = ep_of(ref), **p;
  if (!e)
    return kOTBadReferenceErr;
  drop_inds(e);
  if (e->fd >= 0)
    close(e->fd);
  for (p = &eps; *p; p = &(*p)->next)
    if (*p == e) {
      *p = e->next;
      break;
    }
  e->magic = 0;
  DisposePtr((Ptr)e);
  return kOTNoError;
}

/* Puts a provider in synchronous mode. */
OSStatus OTSetSynchronous(ProviderRef ref) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  e->sync = true;
  return kOTNoError;
}

/* Puts a provider in asynchronous mode: calls end with events to the
   notifier. */
OSStatus OTSetAsynchronous(ProviderRef ref) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  e->sync = false;
  return kOTNoError;
}

/* Tells if a provider is in synchronous mode. */
Boolean OTIsSynchronous(ProviderRef ref) {
  struct ep *e = ep_of(ref);
  return e ? e->sync : false;
}

/* Puts a provider in blocking mode: calls wait until they can be done. */
OSStatus OTSetBlocking(ProviderRef ref) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  e->blocking = true;
  return kOTNoError;
}

/* Puts a provider in nonblocking mode: calls that cannot be done now
   give kOTNoDataErr or kOTFlowErr. */
OSStatus OTSetNonBlocking(ProviderRef ref) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  e->blocking = false;
  return kOTNoError;
}

/* Tells if a provider is in blocking mode. */
Boolean OTIsBlocking(ProviderRef ref) {
  struct ep *e = ep_of(ref);
  return e ? e->blocking : false;
}

/* Sets the notifier of a provider. */
OSStatus OTInstallNotifier(ProviderRef ref, OTNotifyUPP proc, void *contextPtr) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  if (e->proc)
    return kOTStateChangeErr;
  e->proc = proc;
  e->context = contextPtr;
  return kOTNoError;
}

/* Removes the notifier of a provider. */
void OTRemoveNotifier(ProviderRef ref) {
  struct ep *e = ep_of(ref);
  if (e) {
    e->proc = NULL;
    e->nevents = 0;
  }
}

/* Tells the notifier to get kOTSyncIdleEvent while a synchronous call
   waits, or not. */
OSStatus OTUseSyncIdleEvents(ProviderRef ref, Boolean useEvents) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  e->idle_events = useEvents;
  return kOTNoError;
}

/* Stops the notifier of a provider from running. Tells if it did. */
Boolean OTEnterNotifier(ProviderRef ref) {
  struct ep *e = ep_of(ref);
  if (!e)
    return false;
  e->in_notifier++;
  return true;
}

/* Lets the notifier of a provider run again after OTEnterNotifier. */
void OTLeaveNotifier(ProviderRef ref) {
  struct ep *e = ep_of(ref);
  if (e && e->in_notifier)
    e->in_notifier--;
}

/* Asks for T_MEMORYRELEASED after sends. The sends here copy the data,
   so the notifier gets the event at the next OTIdle. */
OSStatus OTAckSends(ProviderRef ref) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  e->acking = true;
  return kOTNoError;
}

/* Stops T_MEMORYRELEASED after sends. */
OSStatus OTDontAckSends(ProviderRef ref) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  e->acking = false;
  return kOTNoError;
}

/* Tells if the provider gives T_MEMORYRELEASED after sends. */
Boolean OTIsAckingSends(ProviderRef ref) {
  struct ep *e = ep_of(ref);
  return e ? e->acking : false;
}

/* Cancels the synchronous calls that wait. The calls here end before
   they return to the program, so there are none. */
OSStatus OTCancelSynchronousCalls(ProviderRef ref, OSStatus err) {
  (void)err;
  return ep_of(ref) ? kOTNoError : kOTBadReferenceErr;
}

/* Does a STREAMS ioctl. There are no STREAMS modules here. */
SInt32 OTIoctl(ProviderRef ref, UInt32 cmd, void *data) {
  (void)ref;
  (void)cmd;
  (void)data;
  return kOTNotSupportedErr;
}

/* Gives the port of a provider. There are no port records here. */
OTPortRef OTGetProviderPortRef(ProviderRef ref) {
  (void)ref;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Endpoint state */

/* Gives the information record of an endpoint. */
OSStatus OTGetEndpointInfo(EndpointRef ref, TEndpointInfo *info) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  fill_info(e->kind, info);
  return kOTNoError;
}

/* Gives the state of an endpoint (T_UNBND, T_IDLE, T_OUTCON, ...). */
OTResult OTGetEndpointState(EndpointRef ref) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  if (e->kind == K_TCP && e->ninds == 0 && e->qlen > 0 && e->state == T_INCON)
    e->state = T_IDLE;
  return e->state;
}

/* Gives the event that waits on an endpoint, or 0. */
OTResult OTLook(EndpointRef ref) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  return look(e);
}

/* Gives the bytes that OTRcv can get now. */
OSStatus OTCountDataBytes(EndpointRef ref, OTByteCount *countPtr) {
  struct ep *e = ep_of(ref);
  int n = 0;
  if (!e)
    return kOTBadReferenceErr;
  if (e->fd < 0 || (e->kind == K_TCP && e->state != T_DATAXFER && e->state != T_OUTREL))
    return kOTOutStateErr;
  if (ioctl(e->fd, FIONREAD, &n) < 0 || n == 0)
    return kOTNoDataErr;
  *countPtr = (OTByteCount)n;
  return kOTNoError;
}

/* Gives the address of an endpoint and the address of the other end. */
OSStatus OTGetProtAddress(EndpointRef ref, TBind *boundAddr, TBind *peerAddr) {
  struct ep *e = ep_of(ref);
  struct sockaddr_in sin;
  socklen_t len;
  if (!e)
    return kOTBadReferenceErr;
  if (boundAddr) {
    memset(&sin, 0, sizeof sin);
    len = sizeof sin;
    if (e->fd >= 0)
      getsockname(e->fd, (struct sockaddr *)&sin, &len);
    put_addr(&boundAddr->addr, &sin);
  }
  if (peerAddr) {
    memset(&sin, 0, sizeof sin);
    len = sizeof sin;
    if (e->fd >= 0 && e->state == T_DATAXFER)
      getpeername(e->fd, (struct sockaddr *)&sin, &len);
    put_addr(&peerAddr->addr, &sin);
  }
  return complete(e, T_GETPROTADDRCOMPLETE, kOTNoError, NULL);
}

/* Resolves an address. TCP and UDP addresses need no resolving. */
OSStatus OTResolveAddress(EndpointRef ref, TBind *reqAddr, TBind *retAddr, OTTimeout timeOut) {
  struct ep *e = ep_of(ref);
  (void)timeOut;
  if (!e)
    return kOTBadReferenceErr;
  if (reqAddr && retAddr && retAddr->addr.buf && retAddr->addr.maxlen >= reqAddr->addr.len) {
    memcpy(retAddr->addr.buf, reqAddr->addr.buf, reqAddr->addr.len);
    retAddr->addr.len = reqAddr->addr.len;
  }
  return complete(e, T_RESOLVEADDRCOMPLETE, kOTNoError, NULL);
}

/* ------------------------------------------------------------------ */
/* Options */

/* Reads or sets options. Here the options are accepted and given back
   as they are: MacTCP has few of them. */
OSStatus OTOptionManagement(EndpointRef ref, TOptMgmt *req, TOptMgmt *ret) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  if (req && ret && ret->opt.buf) {
    OTByteCount n = req->opt.len < ret->opt.maxlen ? req->opt.len : ret->opt.maxlen;
    if (n)
      memcpy(ret->opt.buf, req->opt.buf, n);
    ret->opt.len = n;
    ret->flags = T_SUCCESS;
  }
  return complete(e, T_OPTMGMTCOMPLETE, kOTNoError, ret);
}

/* Gives the option after *prevOptPtr in an options buffer (the first one
   when *prevOptPtr is NULL), or sets *prevOptPtr to NULL at the end. */
OSStatus OTNextOption(UInt8 *buffer, UInt32 buflen, TOption **prevOptPtr) {
  TOption *o = *prevOptPtr;
  UInt8 *next;
  if (!o)
    next = buffer;
  else
    next = (UInt8 *)o + ((o->len + 3) & ~3UL);
  if (next + sizeof(TOptionHeader) > buffer + buflen) {
    *prevOptPtr = NULL;
    return kOTNoError;
  }
  o = (TOption *)next;
  if (o->len < sizeof(TOptionHeader) || next + o->len > buffer + buflen)
    return kOTBadOptionErr;
  *prevOptPtr = o;
  return kOTNoError;
}

/* Finds an option of a level and name in an options buffer, or NULL. */
TOption *OTFindOption(UInt8 *buffer, UInt32 buflen, OTXTILevel level, OTXTIName name) {
  TOption *o = NULL;
  while (OTNextOption(buffer, buflen, &o) == kOTNoError && o)
    if (o->level == level && o->name == name)
      return o;
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Binding and connections */

/* Binds an endpoint to an address (any address when reqAddr is NULL or
   empty). A TCP endpoint with qlen above 0 listens for connections. */
OSStatus OTBind(EndpointRef ref, TBind *reqAddr, TBind *retAddr) {
  struct ep *e = ep_of(ref);
  struct sockaddr_in sin;
  socklen_t len = sizeof sin;
  OTQLen qlen = 0;
  if (!e)
    return kOTBadReferenceErr;
  if (e->state != T_UNBND)
    return kOTOutStateErr;
  memset(&sin, 0, sizeof sin);
  sin.sin_family = AF_INET;
  if (reqAddr) {
    if (reqAddr->addr.len && get_addr(&reqAddr->addr, &sin) < 0)
      return kOTBadAddressErr;
    qlen = reqAddr->qlen;
  }
  if (need_socket(e) < 0)
    return ot_error(errno);
  if (bind(e->fd, (struct sockaddr *)&sin, sizeof sin) < 0 ||
      (e->kind == K_TCP && qlen > 0 && listen(e->fd, qlen) < 0)) {
    OSStatus err = ot_error(errno);
    close(e->fd);
    e->fd = -1;
    return complete(e, T_BINDCOMPLETE, err, NULL);
  }
  e->qlen = e->kind == K_TCP ? qlen : 0;
  if (retAddr) {
    getsockname(e->fd, (struct sockaddr *)&sin, &len);
    put_addr(&retAddr->addr, &sin);
    retAddr->qlen = e->qlen;
  }
  e->state = T_IDLE;
  e->notified = 0;
  return complete(e, T_BINDCOMPLETE, kOTNoError, retAddr);
}

/* Unbinds an endpoint: closes its socket. */
OSStatus OTUnbind(EndpointRef ref) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  if (e->state != T_IDLE)
    return kOTOutStateErr;
  drop_inds(e);
  if (e->fd >= 0)
    close(e->fd);
  e->fd = -1;
  e->qlen = 0;
  e->state = T_UNBND;
  e->peer_closed = 0;
  return complete(e, T_UNBINDCOMPLETE, kOTNoError, NULL);
}

/* Fills the address of a TCall from the other end of a socket. */
static void put_peer(TCall *call, int fd) {
  struct sockaddr_in sin;
  socklen_t len = sizeof sin;
  if (!call)
    return;
  memset(&sin, 0, sizeof sin);
  getpeername(fd, (struct sockaddr *)&sin, &len);
  put_addr(&call->addr, &sin);
  call->opt.len = 0;
  call->udata.len = 0;
}

/* Opens a TCP connection to sndCall's address. In nonblocking mode and
   in asynchronous mode it gives kOTNoDataErr; the connection ends with
   T_CONNECT and OTRcvConnect. */
OSStatus OTConnect(EndpointRef ref, TCall *sndCall, TCall *rcvCall) {
  struct ep *e = ep_of(ref);
  struct sockaddr_in sin;
  if (!e)
    return kOTBadReferenceErr;
  if (e->kind != K_TCP)
    return kOTNotSupportedErr;
  if (e->state != T_IDLE || e->qlen > 0)
    return kOTOutStateErr;
  if (!sndCall || get_addr(&sndCall->addr, &sin) < 0)
    return kOTBadAddressErr;
  if (connect(e->fd, (struct sockaddr *)&sin, sizeof sin) < 0 && errno != EINPROGRESS)
    return ot_error(errno);
  e->state = T_OUTCON;
  e->connecting = 1;
  e->notified = 0;
  if (!e->sync || !e->blocking)
    return kOTNoDataErr;
  while (!connect_ended(e))
    idle(e);
  if (e->state != T_DATAXFER) {
    e->state = T_IDLE;
    return kECONNREFUSEDErr;
  }
  put_peer(rcvCall, e->fd);
  return kOTNoError;
}

/* Ends a connection that OTConnect started, after T_CONNECT. */
OSStatus OTRcvConnect(EndpointRef ref, TCall *call) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  if (e->state != T_OUTCON)
    return kOTOutStateErr;
  while (e->connecting && !connect_ended(e)) {
    if (!e->sync || !e->blocking)
      return kOTNoDataErr;
    idle(e);
  }
  if (e->state != T_DATAXFER) {
    e->state = T_IDLE;
    return kOTLookErr;
  }
  e->state = T_DATAXFER;
  e->notified = 0;
  put_peer(call, e->fd);
  return kOTNoError;
}

/* Gives the next connection that waits on a listener, with its
   sequence number for OTAccept or OTSndDisconnect. */
OSStatus OTListen(EndpointRef ref, TCall *call) {
  struct ep *e = ep_of(ref);
  struct ind *in;
  socklen_t len;
  int fd;
  if (!e)
    return kOTBadReferenceErr;
  if (e->qlen <= 0)
    return kOTOutStateErr;
  if (e->ninds >= MAX_INDS || (OTQLen)e->ninds >= e->qlen)
    return kOTQFullErr;
  in = &e->inds[e->ninds];
  for (;;) {
    len = sizeof in->from;
    fd = accept(e->fd, (struct sockaddr *)&in->from, &len);
    if (fd >= 0)
      break;
    if (errno != EAGAIN || !e->sync || !e->blocking) {
      e->notified &= ~N_LISTEN;
      return ot_error(errno);
    }
    idle(e);
  }
  sock_nonblock(fd);
  in->fd = fd;
  in->seq = ++e->next_seq;
  e->ninds++;
  e->state = T_INCON;
  e->notified &= ~N_LISTEN;
  if (call) {
    put_addr(&call->addr, &in->from);
    call->opt.len = 0;
    call->udata.len = 0;
    call->sequence = in->seq;
  }
  return kOTNoError;
}

/* Finds the waiting connection of a sequence number. Gives its index,
   or -1. */
static int find_ind(struct ep *e, OTSequence seq) {
  int i;
  for (i = 0; i < e->ninds; i++)
    if (e->inds[i].seq == seq)
      return i;
  return -1;
}

/* Takes the waiting connection at index i off the list. Gives its
   socket. */
static int take_ind(struct ep *e, int i) {
  int fd = e->inds[i].fd;
  e->inds[i] = e->inds[--e->ninds];
  if (!e->ninds)
    e->state = T_IDLE;
  return fd;
}

/* Accepts a connection that OTListen gave: the worker endpoint gets it.
   The worker can be unbound or bound with qlen 0. */
OSStatus OTAccept(EndpointRef listener, EndpointRef worker, TCall *call) {
  struct ep *l = ep_of(listener), *w = ep_of(worker);
  int i;
  if (!l || !w)
    return kOTBadReferenceErr;
  if (!call || (i = find_ind(l, call->sequence)) < 0)
    return kOTBadSequenceErr;
  if (w == l || w->kind != K_TCP || (w->state != T_UNBND && w->state != T_IDLE) || w->qlen > 0)
    return kOTOutStateErr;
  if (w->fd >= 0)
    close(w->fd);
  w->fd = take_ind(l, i);
  w->state = T_DATAXFER;
  w->notified = 0;
  w->peer_closed = 0;
  if (w != l && !w->sync)
    post(w, T_PASSCON, kOTNoError, NULL);
  return complete(l, T_ACCEPTCOMPLETE, kOTNoError, worker);
}

/* Aborts a connection, or rejects a waiting one (call->sequence). */
OSStatus OTSndDisconnect(EndpointRef ref, TCall *call) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  if (e->ninds && call) {
    int i = find_ind(e, call->sequence);
    if (i < 0)
      return kOTBadSequenceErr;
    close(take_ind(e, i));
    return complete(e, T_DISCONNECTCOMPLETE, kOTNoError, NULL);
  }
  if (e->state != T_DATAXFER && e->state != T_OUTCON && e->state != T_OUTREL && e->state != T_INREL)
    return kOTOutStateErr;
  {
    /* Abort the connection: close the socket and make a new one on the
       same port. */
    struct sockaddr_in sin;
    socklen_t len = sizeof sin;
    struct linger lg = {1, 0};
    memset(&sin, 0, sizeof sin);
    getsockname(e->fd, (struct sockaddr *)&sin, &len);
    setsockopt(e->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(e->fd);
    e->fd = -1;
    e->connecting = 0;
    e->state = T_UNBND;
    if (need_socket(e) == 0) {
      sin.sin_port = 0;
      if (bind(e->fd, (struct sockaddr *)&sin, sizeof sin) == 0)
        e->state = T_IDLE;
    }
  }
  e->notified = 0;
  return complete(e, T_DISCONNECTCOMPLETE, kOTNoError, NULL);
}

/* Ends the sending side of a connection (TCP FIN). */
OSStatus OTSndOrderlyDisconnect(EndpointRef ref) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  if (e->state != T_DATAXFER && e->state != T_INREL)
    return kOTOutStateErr;
  if (shutdown(e->fd, SHUT_WR) < 0)
    return ot_error(errno);
  if (e->state == T_INREL)
    e->state = T_IDLE;
  else
    e->state = T_OUTREL;
  return kOTNoError;
}

/* Takes a T_DISCONNECT: the connection is over and the endpoint idle. */
OSStatus OTRcvDisconnect(EndpointRef ref, TDiscon *discon) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  if (look(e) != T_DISCONNECT)
    return kOTNoDisconnectErr;
  if (discon) {
    discon->reason = kECONNRESETErr;
    discon->udata.len = 0;
    discon->sequence = 0;
  }
  e->connecting = 0;
  e->state = T_IDLE;
  e->notified = 0;
  return kOTNoError;
}

/* Takes a T_ORDREL: the other end sends no more. */
OSStatus OTRcvOrderlyDisconnect(EndpointRef ref) {
  struct ep *e = ep_of(ref);
  if (!e)
    return kOTBadReferenceErr;
  if (look(e) != T_ORDREL)
    return kOTNoReleaseErr;
  e->peer_closed = 1;
  e->notified &= ~N_ORDREL;
  if (e->state == T_OUTREL)
    e->state = T_IDLE;
  else
    e->state = T_INREL;
  return kOTNoError;
}

/* ------------------------------------------------------------------ */
/* Data */

/* Receives bytes from a connection. Gives the count, kOTNoDataErr when
   there are none in nonblocking mode, or kOTLookErr at the end of the
   connection (OTLook then gives T_ORDREL or T_DISCONNECT). */
OTResult OTRcv(EndpointRef ref, void *buf, OTByteCount nbytes, OTFlags *flags) {
  struct ep *e = ep_of(ref);
  long n;
  if (!e)
    return kOTBadReferenceErr;
  if (e->kind != K_TCP)
    return kOTNotSupportedErr;
  if (e->state != T_DATAXFER && e->state != T_OUTREL)
    return kOTOutStateErr;
  if (flags)
    *flags = 0;
  for (;;) {
    n = recv(e->fd, buf, nbytes, MSG_DONTWAIT);
    if (n > 0)
      return n;
    if (n == 0)
      return kOTLookErr;
    if (errno != EAGAIN)
      return kOTLookErr;
    e->notified &= ~N_DATA;
    if (!e->sync || !e->blocking)
      return kOTNoDataErr;
    idle(e);
  }
}

/* Sends bytes on a connection. Gives the count sent. */
OTResult OTSnd(EndpointRef ref, void *buf, OTByteCount nbytes, OTFlags flags) {
  struct ep *e = ep_of(ref);
  long n;
  (void)flags;
  if (!e)
    return kOTBadReferenceErr;
  if (e->kind != K_TCP)
    return kOTNotSupportedErr;
  if (e->state != T_DATAXFER && e->state != T_INREL)
    return kOTOutStateErr;
  for (;;) {
    n = send(e->fd, buf, nbytes, 0);
    if (n >= 0)
      break;
    if (errno != EAGAIN)
      return kOTLookErr;
    if (!e->sync || !e->blocking)
      return kOTFlowErr;
    idle(e);
  }
  if (e->acking)
    post(e, T_MEMORYRELEASED, (OTResult)nbytes, buf);
  return n;
}

/* Sends one datagram to udata->addr. */
OSStatus OTSndUData(EndpointRef ref, TUnitData *udata) {
  struct ep *e = ep_of(ref);
  struct sockaddr_in sin;
  if (!e)
    return kOTBadReferenceErr;
  if (e->kind != K_UDP)
    return kOTNotSupportedErr;
  if (e->state != T_IDLE)
    return kOTOutStateErr;
  if (!udata || get_addr(&udata->addr, &sin) < 0)
    return kOTBadAddressErr;
  if (sendto(e->fd, udata->udata.buf, udata->udata.len, 0, (struct sockaddr *)&sin, sizeof sin) < 0)
    return ot_error(errno);
  if (e->acking)
    post(e, T_MEMORYRELEASED, (OTResult)udata->udata.len, udata->udata.buf);
  return kOTNoError;
}

/* Receives one datagram, with the address it came from. */
OSStatus OTRcvUData(EndpointRef ref, TUnitData *udata, OTFlags *flags) {
  struct ep *e = ep_of(ref);
  struct sockaddr_in sin;
  socklen_t len;
  long n;
  if (!e)
    return kOTBadReferenceErr;
  if (e->kind != K_UDP)
    return kOTNotSupportedErr;
  if (e->state != T_IDLE)
    return kOTOutStateErr;
  if (flags)
    *flags = 0;
  for (;;) {
    len = sizeof sin;
    memset(&sin, 0, sizeof sin);
    n = recvfrom(e->fd, udata->udata.buf, udata->udata.maxlen, MSG_DONTWAIT, (struct sockaddr *)&sin,
                 &len);
    if (n >= 0)
      break;
    if (errno != EAGAIN)
      return ot_error(errno);
    e->notified &= ~N_DATA;
    if (!e->sync || !e->blocking)
      return kOTNoDataErr;
    idle(e);
  }
  udata->udata.len = (OTByteCount)n;
  put_addr(&udata->addr, &sin);
  udata->opt.len = 0;
  return kOTNoError;
}

/* Gives the error of a datagram that was not sent. MacTCP reports none. */
OSStatus OTRcvUDErr(EndpointRef ref, TUDErr *uderr) {
  (void)uderr;
  return ep_of(ref) ? kOTNoUDErrErr : kOTBadReferenceErr;
}

/* ------------------------------------------------------------------ */
/* Internet services */

/* Reads a dotted address ("10.0.2.15") into *host. */
OSStatus OTInetStringToHost(const char *str, InetHost *host) {
  unsigned long parts[4] = {0, 0, 0, 0};
  int i = 0, digits = 0;
  for (;; str++) {
    if (*str >= '0' && *str <= '9') {
      parts[i] = parts[i] * 10 + (unsigned long)(*str - '0');
      if (parts[i] > 255 || ++digits > 3)
        return kOTBadNameErr;
    } else if (*str == '.' && digits && i < 3) {
      i++;
      digits = 0;
    } else if ((*str == 0 || *str == ' ') && digits && i == 3) {
      break;
    } else {
      return kOTBadNameErr;
    }
  }
  *host = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
  return kOTNoError;
}

/* Writes an address as dotted text into str (16 bytes). */
void OTInetHostToString(InetHost host, char *str) {
  int i;
  char *p = str;
  for (i = 24; i >= 0; i -= 8) {
    unsigned v = (unsigned)(host >> i) & 0xFF;
    if (v >= 100)
      *p++ = (char)('0' + v / 100);
    if (v >= 10)
      *p++ = (char)('0' + v / 10 % 10);
    *p++ = (char)('0' + v % 10);
    if (i)
      *p++ = '.';
  }
  *p = 0;
}

/* Gives the information of the interface (only interface 0 or
   kDefaultInetInterface): the address, the net mask of its class and
   the broadcast address. */
OSStatus OTInetGetInterfaceInfo(InetInterfaceInfo *info, SInt32 val) {
  InetHost a;
  if (val != 0 && val != kDefaultInetInterface)
    return kOTNotFoundErr;
  a = ntohl(__mac_ip_address());
  if (!a)
    return kOTNotFoundErr;
  memset(info, 0, sizeof *info);
  info->fAddress = a;
  info->fNetmask = a < 0x80000000UL ? 0xFF000000UL : a < 0xC0000000UL ? 0xFFFF0000UL : 0xFFFFFF00UL;
  info->fBroadcastAddr = a | ~info->fNetmask;
  info->fVersion = kInetInterfaceInfoVersion;
  info->fIfMTU = 1500;
  return kOTNoError;
}

/* Gives the secondary addresses of an interface. MacTCP has none. */
OSStatus OTInetGetSecondaryAddresses(InetHost *addr, UInt32 *count, SInt32 val) {
  (void)addr;
  (void)val;
  *count = 0;
  return kOTNoError;
}

/* Waits for a resolver call that gave cacheFault: its rtnCode changes
   when it ends. */
static long dnr_wait(struct ep *e, long rc, volatile hostInfo *hi) {
  if (rc != cacheFault)
    return rc;
  while (hi->rtnCode == cacheFault || hi->rtnCode == inProgress)
    idle(e);
  return hi->rtnCode;
}

/* Finds the addresses of a name (or reads a dotted address). In
   asynchronous mode the notifier gets T_DNRSTRINGTOADDRCOMPLETE with
   hinfo as the cookie. */
OSStatus OTInetStringToAddress(InetSvcRef ref, char *name, InetHostInfo *hinfo) {
  struct ep *e = ep_of(ref);
  static hostInfo hi;
  OSStatus err;
  InetHost h;
  int i;
  if (!e || e->kind != K_INET)
    return kOTBadReferenceErr;
  memset(hinfo, 0, sizeof *hinfo);
  if (OTInetStringToHost(name, &h) == kOTNoError) {
    strncpy(hinfo->name, name, sizeof hinfo->name - 1);
    hinfo->addrs[0] = h;
    return complete(e, T_DNRSTRINGTOADDRCOMPLETE, kOTNoError, hinfo);
  }
  if (OpenResolver(NULL) != noErr)
    return complete(e, T_DNRSTRINGTOADDRCOMPLETE, kOTNotFoundErr, hinfo);
  memset(&hi, 0, sizeof hi);
  hi.rtnCode = cacheFault;
  err = dnr_error(dnr_wait(e, StrToAddr(name, &hi, NULL, NULL), &hi));
  if (err == kOTNoError) {
    strncpy(hinfo->name, hi.cname[0] ? hi.cname : name, sizeof hinfo->name - 1);
    for (i = 0; i < 4 && hi.addr[i]; i++)
      hinfo->addrs[i] = hi.addr[i];
  }
  return complete(e, T_DNRSTRINGTOADDRCOMPLETE, err, hinfo);
}

/* Finds the name of an address (a name of 256 bytes). In asynchronous
   mode the notifier gets T_DNRADDRTONAMECOMPLETE with name as the
   cookie. */
OSStatus OTInetAddressToName(InetSvcRef ref, InetHost addr, char *name) {
  struct ep *e = ep_of(ref);
  static hostInfo hi;
  OSStatus err;
  if (!e || e->kind != K_INET)
    return kOTBadReferenceErr;
  if (OpenResolver(NULL) != noErr)
    return complete(e, T_DNRADDRTONAMECOMPLETE, kOTNotFoundErr, name);
  memset(&hi, 0, sizeof hi);
  hi.rtnCode = cacheFault;
  err = dnr_error(dnr_wait(e, AddrToName(addr, &hi, NULL, NULL), &hi));
  if (err == kOTNoError) {
    strncpy(name, hi.cname, kMaxHostNameLen);
    name[kMaxHostNameLen] = 0;
  }
  return complete(e, T_DNRADDRTONAMECOMPLETE, err, name);
}

/* Finds the processor and system of a host (the HINFO record). */
OSStatus OTInetSysInfo(InetSvcRef ref, char *name, InetSysInfo *sysinfo) {
  struct ep *e = ep_of(ref);
  static returnRec rr;
  long rc;
  OSStatus err;
  if (!e || e->kind != K_INET)
    return kOTBadReferenceErr;
  if (OpenResolver(NULL) != noErr)
    return complete(e, T_DNRSYSINFOCOMPLETE, kOTNotFoundErr, sysinfo);
  memset(&rr, 0, sizeof rr);
  rr.rtnCode = cacheFault;
  rc = HInfo(name, &rr, NULL, NULL);
  if (rc == cacheFault)
    while (((volatile returnRec *)&rr)->rtnCode == cacheFault ||
           ((volatile returnRec *)&rr)->rtnCode == inProgress)
      idle(e);
  else
    rr.rtnCode = rc;
  err = dnr_error(rr.rtnCode);
  if (err == kOTNoError) {
    memcpy(sysinfo->cpuType, rr.rdata.hinfo.cpuType, sizeof sysinfo->cpuType);
    memcpy(sysinfo->osType, rr.rdata.hinfo.osType, sizeof sysinfo->osType);
  }
  return complete(e, T_DNRSYSINFOCOMPLETE, err, sysinfo);
}

/* Finds the mail exchanges of a domain (the MX records). MacTCP's
   resolver gives one; *num gets 1. */
OSStatus OTInetMailExchange(InetSvcRef ref, char *name, UInt16 *num, InetMailExchange *mx) {
  struct ep *e = ep_of(ref);
  static returnRec rr;
  long rc;
  OSStatus err;
  if (!e || e->kind != K_INET)
    return kOTBadReferenceErr;
  if (OpenResolver(NULL) != noErr)
    return complete(e, T_DNRMAILEXCHANGECOMPLETE, kOTNotFoundErr, mx);
  memset(&rr, 0, sizeof rr);
  rr.rtnCode = cacheFault;
  rc = MXInfo(name, &rr, NULL, NULL);
  if (rc == cacheFault)
    while (((volatile returnRec *)&rr)->rtnCode == cacheFault ||
           ((volatile returnRec *)&rr)->rtnCode == inProgress)
      idle(e);
  else
    rr.rtnCode = rc;
  err = dnr_error(rr.rtnCode);
  if (err == kOTNoError && *num > 0) {
    mx[0].preference = rr.rdata.mx.preference;
    strncpy(mx[0].exchange, rr.rdata.mx.exchange, kMaxHostNameLen);
    mx[0].exchange[kMaxHostNameLen] = 0;
    *num = 1;
  } else {
    *num = 0;
  }
  return complete(e, T_DNRMAILEXCHANGECOMPLETE, err, mx);
}

/* Sends a general DNS query. MacTCP's resolver cannot. */
OSStatus OTInetQuery(InetSvcRef ref, char *name, UInt16 qClass, UInt16 qType, char *buf,
                     OTByteCount buflen, void **argv, OTByteCount argvlen, OTFlags requestFlags) {
  (void)name;
  (void)qClass;
  (void)qType;
  (void)buf;
  (void)buflen;
  (void)argv;
  (void)argvlen;
  (void)requestFlags;
  return ep_of(ref) ? kOTNotSupportedErr : kOTBadReferenceErr;
}

/* Fills an Internet address. */
void OTInitInetAddress(InetAddress *addr, InetPort port, InetHost host) {
  addr->fAddressType = AF_INET;
  addr->fPort = port;
  addr->fHost = host;
  memset(addr->fUnused, 0, sizeof addr->fUnused);
}

/* Fills a DNS address (a name and a port, "host:port"). Gives its size. */
OTByteCount OTInitDNSAddress(DNSAddress *addr, char *str) {
  OTByteCount n = strlen(str);
  if (n > sizeof addr->fName - 1)
    n = sizeof addr->fName - 1;
  addr->fAddressType = AF_DNS;
  memcpy(addr->fName, str, n);
  addr->fName[n] = 0;
  return n + sizeof addr->fAddressType + 1;
}

#endif
