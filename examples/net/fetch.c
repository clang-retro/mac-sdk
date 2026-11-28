/* Fetch: gets a web page twice, once with Open Transport and once with
 * BSD sockets, and shows the first line of each reply in a console
 * window. It builds for 68k and PowerPC from this one source.
 *
 * On PowerPC the Open Transport calls go to Mac OS's Open Transport. On
 * 68k they go to Apple's 68k Open Transport through the Shared Library
 * Manager, or to the SDK's Open Transport over MacTCP. The socket calls
 * use MacTCP's driver on both, which Open Transport also supplies. So
 * the program runs on System 7 with MacTCP and on Mac OS 7.6 to 9 with
 * Open Transport.
 *
 *   python3 tools/macapp.py --console window -o Fetch.bin -- examples/net/fetch.c
 *
 * Define FETCH_HOST and FETCH_PORT to get a page from another server
 * (tools/qemu/nettest.py uses the QEMU host, 10.0.2.2).
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <Events.h>
#include <OpenTransport.h>
#include <OpenTransportProviders.h>

#ifndef FETCH_HOST
#define FETCH_HOST "example.com"
#endif
#ifndef FETCH_PORT
#define FETCH_PORT 80
#endif
#define WAIT_TICKS (60UL * 20) /* the longest wait for a reply: 20 seconds */

static char reply[4096]; /* the start of a reply */

/* Makes the request, with a User-Agent that names the interface. Gives
   its length. */
static int request(char *buf, size_t size, const char *api) {
  return snprintf(buf, size,
                  "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: clang-retro-fetch (%s)\r\n"
                  "Connection: close\r\n\r\n",
                  FETCH_HOST, api);
}

/* Shows the first line of a reply of n bytes (total bytes in all). */
static void show(const char *api, long n, long total) {
  char *end;
  if (n <= 0) {
    printf("%s: no reply\n", api);
    return;
  }
  reply[n < (long)sizeof reply ? n : (long)sizeof reply - 1] = 0;
  end = strpbrk(reply, "\r\n");
  if (end)
    *end = 0;
  printf("%s: %s (%ld bytes)\n", api, reply, total);
}

/* Lets the other programs run for a moment, and lets Open Transport's
   notifiers run on 68k. */
static void pause_a_moment(void) {
  EventRecord ev;
  OTIdle();
  EventAvail(0, &ev);
}

/* Gets the page with Open Transport. The endpoint is synchronous and
   nonblocking: the program waits in its own loop and lets other
   programs run. */
static void fetch_ot(void) {
  OSStatus err;
  InetSvcRef svc;
  InetHostInfo host;
  EndpointRef ep;
  InetAddress addr;
  TCall call;
  OTFlags flags;
  OTResult n;
  long got = 0;
  unsigned long until;
  char req[256];
  if (InitOpenTransport() != kOTNoError) {
    printf("Open Transport: not here\n");
    return;
  }
  svc = OTOpenInternetServices(kDefaultInternetServicesPath, 0, &err);
  if (err != kOTNoError) {
    printf("Open Transport: no Internet services (%ld)\n", (long)err);
    return;
  }
  err = OTInetStringToAddress(svc, (char *)FETCH_HOST, &host);
  OTCloseProvider(svc);
  if (err != kOTNoError) {
    printf("Open Transport: cannot find %s (%ld)\n", FETCH_HOST, (long)err);
    return;
  }
  ep = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err);
  if (err != kOTNoError) {
    printf("Open Transport: no TCP endpoint (%ld)\n", (long)err);
    return;
  }
  OTSetBlocking(ep);
  err = OTBind(ep, NULL, NULL);
  if (err == kOTNoError) {
    OTInitInetAddress(&addr, FETCH_PORT, host.addrs[0]);
    memset(&call, 0, sizeof call);
    call.addr.buf = (UInt8 *)&addr;
    call.addr.len = sizeof addr;
    err = OTConnect(ep, &call, NULL);
  }
  if (err == kOTNoError) {
    int len = request(req, sizeof req, "Open Transport");
    if (OTSnd(ep, req, len, 0) != len)
      err = kOTLookErr;
  }
  if (err != kOTNoError) {
    printf("Open Transport: cannot connect (%ld)\n", (long)err);
    OTCloseProvider(ep);
    return;
  }
  /* Read until the server closes the connection. */
  OTSetNonBlocking(ep);
  until = TickCount() + WAIT_TICKS;
  for (;;) {
    char buf[512];
    n = OTRcv(ep, buf, sizeof buf, &flags);
    if (n > 0) {
      if (got < (long)sizeof reply - 1)
        memcpy(reply + got, buf, (size_t)n < sizeof reply - 1 - got ? (size_t)n : sizeof reply - 1 - got);
      got += n;
    } else if (n == kOTNoDataErr && TickCount() < until) {
      pause_a_moment();
    } else {
      if (n == kOTLookErr && OTLook(ep) == T_ORDREL)
        OTRcvOrderlyDisconnect(ep);
      break;
    }
  }
  show("Open Transport", got < (long)sizeof reply ? got : (long)sizeof reply - 1, got);
  OTCloseProvider(ep);
}

/* Gets the page with BSD sockets. */
static void fetch_sockets(void) {
  struct hostent *h = gethostbyname(FETCH_HOST);
  struct sockaddr_in sin;
  char req[256];
  long got = 0, n;
  int fd, len;
  if (!h) {
    printf("Sockets: cannot find %s\n", FETCH_HOST);
    return;
  }
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    printf("Sockets: no socket\n");
    return;
  }
  memset(&sin, 0, sizeof sin);
  sin.sin_family = AF_INET;
  sin.sin_port = htons(FETCH_PORT);
  memcpy(&sin.sin_addr, h->h_addr_list[0], sizeof sin.sin_addr);
  len = request(req, sizeof req, "BSD sockets");
  if (connect(fd, (struct sockaddr *)&sin, sizeof sin) < 0 || send(fd, req, len, 0) != len) {
    printf("Sockets: cannot connect\n");
    close(fd);
    return;
  }
  /* recv waits and lets other programs run meanwhile. */
  while ((n = recv(fd, reply + got, sizeof reply - 1 - got, 0)) > 0) {
    got += n;
    if (got >= (long)sizeof reply - 1)
      break;
  }
  show("BSD sockets", got, got);
  close(fd);
}

/* Gets the page both ways. */
int main(void) {
  printf("Fetching http://%s:%d/\n", FETCH_HOST, FETCH_PORT);
  fetch_ot();
  fetch_sockets();
  CloseOpenTransport();
  return 0;
}
