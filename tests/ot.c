/* Checks the Open Transport calls: the utilities, the Internet services
 * (addresses and names), a TCP connection between two endpoints of this
 * program (a listener, a client and the endpoint that accepts), a UDP
 * datagram to this program, and an endpoint that a notifier follows.
 *
 * On PowerPC the calls go to Open Transport (runtime/libc/ot.c). On 68k
 * they go to native Open Transport through the Apple Shared Library
 * Manager (runtime/libc/otnative.c) when it is installed, else to the
 * SDK's Open Transport over MacTCP (runtime/libc/ot68k.c).
 * tests/run.sh runs it in tools/sim68k.py --os, whose fake MacTCP uses
 * the host's loopback network; tools/qemu/nettest.py runs it on Mac OS
 * 8.1 (68k) and 9.2.1 (PowerPC) in QEMU. The output has one "ok" line
 * per check, then "all ok". With a report server at 10.0.2.2 port 7009
 * (the QEMU host), the lines also go there.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <OpenTransport.h>
#include <OpenTransportProviders.h>
#include <Events.h>

#define TCP_PORT 7101 /* the listener's port */

static int failures;
static EndpointRef report; /* the report connection, or NULL */
static char early[2048];   /* the lines before the report connection */
static size_t nearly;

/* Writes one line to the console and to the report connection. */
static void say(const char *fmt, ...) {
  char line[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line - 1, fmt, ap);
  va_end(ap);
  printf("%s\n", line);
  fflush(stdout);
  {
    size_t n = strlen(line);
    line[n++] = '\n';
    if (report)
      OTSnd(report, line, n, 0);
    else if (nearly + n <= sizeof early) {
      memcpy(early + nearly, line, n);
      nearly += n;
    }
  }
}

/* Prints one check's result, and counts the failures. */
static void check(const char *what, int ok) {
  say("%s %s", ok ? "ok" : "FAILED", what);
  if (!ok)
    failures++;
}

/* Waits until cond is true or about secs seconds pass, and lets other
   programs run. Gives cond. */
#define WAIT_FOR(cond, secs)                                                                       \
  do {                                                                                             \
    unsigned long until_ = TickCount() + 60UL * (secs);                                            \
    while (!(cond) && TickCount() < until_) {                                                      \
      EventRecord ev_;                                                                             \
      OTIdle();                                                                                    \
      EventAvail(0, &ev_);                                                                         \
    }                                                                                              \
  } while (0)

/* Opens an endpoint of a configuration name ("tcp" or "udp"), and puts
   it in blocking mode (an endpoint starts synchronous and nonblocking).
   Gives NULL on an error. */
static EndpointRef open_ep(const char *name, TEndpointInfo *info) {
  OSStatus err = -1;
  EndpointRef ep = OTOpenEndpoint(OTCreateConfiguration(name), 0, info, &err);
  if (err != kOTNoError) {
    say("note: OTOpenEndpoint %s: error %ld", name, (long)err);
    return NULL;
  }
  OTSetBlocking(ep);
  return ep;
}

/* Binds ep to port (0: any) with qlen waiting connections. Gives the
   bound port, or 0. */
static InetPort bind_ep(EndpointRef ep, InetPort port, OTQLen qlen) {
  InetAddress want, got;
  TBind req, ret;
  OTInitInetAddress(&want, port, kOTAnyInetAddress);
  memset(&req, 0, sizeof req);
  memset(&ret, 0, sizeof ret);
  req.addr.buf = (UInt8 *)&want;
  req.addr.len = sizeof want;
  req.qlen = qlen;
  ret.addr.buf = (UInt8 *)&got;
  ret.addr.maxlen = sizeof got;
  if (OTBind(ep, &req, &ret) != kOTNoError)
    return 0;
  return got.fPort;
}

/* Connects a report endpoint to the QEMU host, when one listens. */
static void open_report(InetHost me) {
  InetAddress host;
  TCall call;
  EndpointRef ep;
  if ((me & 0xFFFFFF00UL) != 0x0A000200UL) /* QEMU's user network */
    return;
  ep = open_ep(kTCPName, NULL);
  if (!ep)
    return;
  if (bind_ep(ep, 0, 0) == 0) {
    OTCloseProvider(ep);
    return;
  }
  OTInitInetAddress(&host, 7009, 0x0A000202UL);
  memset(&call, 0, sizeof call);
  call.addr.buf = (UInt8 *)&host;
  call.addr.len = sizeof host;
  if (OTConnect(ep, &call, NULL) == kOTNoError) {
    report = ep;
    OTSnd(report, early, nearly, 0);
  } else
    OTCloseProvider(ep);
}

/* The utility calls: memory, strings, time. */
static void check_utilities(void) {
  char a[16], b[16];
  OTTimeStamp start;
  void *mem;
  OTMemset(a, 'x', sizeof a);
  OTMemcpy(b, a, sizeof a);
  check("OTMemcpy, OTMemcmp", OTMemcmp(a, b, sizeof a) && a[15] == 'x');
  OTMemzero(a, 4);
  check("OTMemzero", a[0] == 0 && a[3] == 0 && a[4] == 'x');
  OTStrCopy(a, "open");
  OTStrCat(a, "tpt");
  check("OTStrCopy, OTStrCat, OTStrLength", OTStrLength(a) == 7 && OTStrEqual(a, "opentpt"));
  OTGetTimeStamp(&start);
  check("OTElapsedMilliseconds", OTElapsedMilliseconds(&start) < 5000);
  mem = OTAllocMem(100);
  check("OTAllocMem", mem != NULL);
  if (mem)
    OTFreeMem(mem);
}

/* Address text, the interface, and the name resolver. Gives this
   Mac's address. */
static InetHost check_inet(void) {
  InetHost h = 0;
  InetInterfaceInfo info;
  InetHostInfo hinfo;
  InetSvcRef svc;
  OSStatus err = -1;
  char text[32];
  check("OTInetStringToHost", OTInetStringToHost("10.0.2.15", &h) == kOTNoError && h == 0x0A00020FUL);
  OTInetHostToString(0xC0A80001UL, text);
  check("OTInetHostToString", strcmp(text, "192.168.0.1") == 0);
  memset(&info, 0, sizeof info);
  err = OTInetGetInterfaceInfo(&info, kDefaultInetInterface);
  check("OTInetGetInterfaceInfo", err == kOTNoError && info.fAddress != 0);
  OTInetHostToString(info.fAddress, text);
  say("note: address %s", text);
  open_report(info.fAddress);
  svc = OTOpenInternetServices(kDefaultInternetServicesPath, 0, &err);
  check("OTOpenInternetServices", err == kOTNoError && svc != NULL);
  if (err == kOTNoError) {
    memset(&hinfo, 0, sizeof hinfo);
    err = OTInetStringToAddress(svc, (char *)"10.0.2.2", &hinfo);
    check("OTInetStringToAddress (a number)", err == kOTNoError && hinfo.addrs[0] == 0x0A000202UL);
    memset(&hinfo, 0, sizeof hinfo);
    err = OTInetStringToAddress(svc, (char *)"example.com", &hinfo);
    say("note: example.com: result %ld, %08lx", (long)err, (unsigned long)hinfo.addrs[0]);
    OTCloseProvider(svc);
  }
  return info.fAddress;
}

/* Receives want bytes from ep in nonblocking mode, and lets other
   programs run while it waits (up to 10 seconds). Gives the count. */
static int rcv_all(EndpointRef ep, char *buf, int want) {
  OTFlags flags = 0;
  OTResult n = kOTNoDataErr;
  int got = 0;
  OTSetNonBlocking(ep);
  while (got < want) {
    WAIT_FOR((n = OTRcv(ep, buf + got, want - got, &flags)) != kOTNoDataErr, 10);
    if (n <= 0)
      break;
    got += n;
  }
  OTSetBlocking(ep);
  return got;
}

/* A TCP connection between two endpoints of this program. */
static void check_tcp(InetHost me) {
  EndpointRef listener, client, worker;
  TEndpointInfo info;
  InetAddress to, from;
  TCall call;
  OTByteCount count = 0;
  char buf[64];
  OTResult n;
  int got;
  listener = open_ep(kTCPName, &info);
  check("OTOpenEndpoint tcp", listener != NULL && info.servtype == T_COTS_ORD);
  if (!listener)
    return;
  check("OTBind (listener)", bind_ep(listener, TCP_PORT, 1) == TCP_PORT);
  check("OTGetEndpointState (idle)", OTGetEndpointState(listener) == T_IDLE);
  client = open_ep(kTCPName, NULL);
  worker = open_ep(kTCPName, NULL);
  check("OTBind (client, worker)", client && worker && bind_ep(client, 0, 0) != 0 &&
                                       bind_ep(worker, 0, 0) != 0);
  OTSetNonBlocking(client);
  OTInitInetAddress(&to, TCP_PORT, me);
  memset(&call, 0, sizeof call);
  call.addr.buf = (UInt8 *)&to;
  call.addr.len = sizeof to;
  n = OTConnect(client, &call, NULL);
  check("OTConnect (nonblocking)", n == kOTNoDataErr || n == kOTNoError);
  memset(&call, 0, sizeof call);
  call.addr.buf = (UInt8 *)&from;
  call.addr.maxlen = sizeof from;
  check("OTListen", OTListen(listener, &call) == kOTNoError && from.fHost == me);
  check("OTAccept", OTAccept(listener, worker, &call) == kOTNoError);
  if (n == kOTNoDataErr) {
    WAIT_FOR(OTLook(client) == T_CONNECT, 10);
    check("OTRcvConnect", OTRcvConnect(client, NULL) == kOTNoError);
  }
  check("connected", OTGetEndpointState(client) == T_DATAXFER &&
                         OTGetEndpointState(worker) == T_DATAXFER);
  OTSetBlocking(client);
  check("OTSnd", OTSnd(client, (void *)"hello, worker", 13, 0) == 13);
  got = rcv_all(worker, buf, 13);
  check("OTRcv", got == 13 && memcmp(buf, "hello, worker", 13) == 0);
  check("OTSnd (back)", OTSnd(worker, (void *)"reply", 5, 0) == 5);
  WAIT_FOR(OTCountDataBytes(client, &count) == kOTNoError && count >= 5, 10);
  check("OTCountDataBytes", count == 5);
  check("OTRcv (back)", rcv_all(client, buf, 5) == 5 && memcmp(buf, "reply", 5) == 0);
  check("OTSndOrderlyDisconnect", OTSndOrderlyDisconnect(client) == kOTNoError);
  WAIT_FOR(OTLook(worker) == T_ORDREL, 10);
  check("T_ORDREL", OTRcvOrderlyDisconnect(worker) == kOTNoError);
  check("OTSndOrderlyDisconnect (back)", OTSndOrderlyDisconnect(worker) == kOTNoError);
  WAIT_FOR(OTLook(client) == T_ORDREL, 10);
  check("T_ORDREL (back)", OTRcvOrderlyDisconnect(client) == kOTNoError);
  check("OTUnbind", OTUnbind(client) == kOTNoError && OTGetEndpointState(client) == T_UNBND);
  OTCloseProvider(client);
  OTCloseProvider(worker);
  OTCloseProvider(listener);
}

/* A UDP datagram from one endpoint of this program to another. */
static void check_udp(InetHost me) {
  EndpointRef a, b;
  TEndpointInfo info;
  InetAddress to, from;
  TUnitData ud;
  OTFlags flags = 0;
  OSStatus err = kOTNoDataErr;
  char buf[64];
  InetPort port;
  a = open_ep(kUDPName, &info);
  b = open_ep(kUDPName, NULL);
  check("OTOpenEndpoint udp", a && b && info.servtype == T_CLTS);
  if (!a || !b)
    return;
  port = bind_ep(b, 0, 0);
  check("OTBind udp", bind_ep(a, 0, 0) != 0 && port != 0);
  OTInitInetAddress(&to, port, me);
  memset(&ud, 0, sizeof ud);
  ud.addr.buf = (UInt8 *)&to;
  ud.addr.len = sizeof to;
  ud.udata.buf = (UInt8 *)"datagram";
  ud.udata.len = 8;
  check("OTSndUData", OTSndUData(a, &ud) == kOTNoError);
  OTSetNonBlocking(b);
  memset(&ud, 0, sizeof ud);
  ud.addr.buf = (UInt8 *)&from;
  ud.addr.maxlen = sizeof from;
  ud.udata.buf = (UInt8 *)buf;
  ud.udata.maxlen = sizeof buf;
  WAIT_FOR((err = OTRcvUData(b, &ud, &flags)) != kOTNoDataErr, 10);
  check("OTRcvUData", err == kOTNoError && ud.udata.len == 8 && memcmp(buf, "datagram", 8) == 0 &&
                          from.fHost == me);
  OTCloseProvider(a);
  OTCloseProvider(b);
}

static volatile EndpointRef async_ep;
static volatile OTResult async_result = 1;
static volatile int bound;

/* The notifier of the asynchronous endpoint. */
static pascal void notifier(void *context, OTEventCode code, OTResult result, void *cookie) {
  (void)context;
  if (code == T_OPENCOMPLETE) {
    async_result = result;
    async_ep = (EndpointRef)cookie;
  } else if (code == T_BINDCOMPLETE) {
    bound = result == kOTNoError ? 1 : -1;
  }
}

/* An endpoint opened and bound asynchronously. */
static void check_async(void) {
  OSStatus err;
  err = OTAsyncOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, NewOTNotifyUPP(notifier),
                            NULL);
  check("OTAsyncOpenEndpoint", err == kOTNoError);
  WAIT_FOR(async_result != 1, 10);
  check("T_OPENCOMPLETE", async_result == kOTNoError && async_ep != NULL);
  if (async_ep) {
    check("OTIsSynchronous (no)", !OTIsSynchronous(async_ep));
    check("OTBind (asynchronous)", OTBind(async_ep, NULL, NULL) == kOTNoError);
    WAIT_FOR(bound != 0, 10);
    check("T_BINDCOMPLETE", bound == 1);
    OTCloseProvider(async_ep);
  }
}

/* Runs the checks. */
int main(void) {
  InetHost me;
  OSStatus err = InitOpenTransport();
  check("InitOpenTransport", err == kOTNoError);
  if (err != kOTNoError)
    return 1;
#if defined(__m68k__)
  {
    int __ot68k_native(void);
    say("note: %s", __ot68k_native() ? "native Open Transport (through ASLM)" : "Open Transport over MacTCP");
  }
#endif
  check_utilities();
  me = check_inet();
  if (me) {
    check_tcp(me);
    check_udp(me);
  }
  check_async();
  say(failures ? "%d FAILED" : "all ok", failures);
  if (report) {
    OTSndOrderlyDisconnect(report);
    OTCloseProvider(report);
  }
  CloseOpenTransport();
  return failures != 0;
}
