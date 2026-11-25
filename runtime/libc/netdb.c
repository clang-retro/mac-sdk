/* Names and addresses: inet_addr, inet_ntop and the others,
 * getaddrinfo, getnameinfo, gethostbyname, gethostbyaddr, and the
 * service and protocol names.
 *
 * A host name is first an address in dotted form, then "localhost",
 * then the Mac's own name (gethostname). Other names go to the domain
 * name resolver of MacTCP or Open Transport: the 'dnrp' code resource
 * of the MacTCP control panel, or of the System file. The resolver is
 * 68k code; the PowerPC calls it through the Mixed Mode Manager. Its
 * calls are asynchronous: they end through a result procedure, and
 * meanwhile the other threads and programs run.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <Folders.h>
#include <Files.h>
#include <Multiverse.h>
#include <MacMemory.h>
#include <MixedMode.h>
#include <Resources.h>
#include "macos.h"
#include "socket.h"

int h_errno;

const struct in6_addr in6addr_any = {{{0}}};
const struct in6_addr in6addr_loopback = {{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}}};

/* ------------------------------------------------------------------ */
/* Address text */

/* Reads a dotted IPv4 address ("a.b.c.d", also "a.b.c", "a.b" and "a",
   in decimal, octal or hexadecimal, as inet_aton does). Gives 1 when
   the text is one. */
int inet_aton(const char *text, struct in_addr *addr) {
  unsigned long part[4];
  int n = 0;
  const char *p = text;
  in_addr_t v;
  if (!text || !*text)
    return 0;
  for (;;) {
    char *end;
    if (*p < '0' || *p > '9')
      return 0;
    part[n] = strtoul(p, &end, 0);
    p = end;
    n++;
    if (*p == '\0')
      break;
    if (*p != '.' || n == 4)
      return 0;
    p++;
  }
  switch (n) {
  case 1:
    v = part[0];
    break;
  case 2:
    if (part[0] > 255 || part[1] > 0xFFFFFF)
      return 0;
    v = (part[0] << 24) | part[1];
    break;
  case 3:
    if (part[0] > 255 || part[1] > 255 || part[2] > 0xFFFF)
      return 0;
    v = (part[0] << 24) | (part[1] << 16) | part[2];
    break;
  default:
    if (part[0] > 255 || part[1] > 255 || part[2] > 255 || part[3] > 255)
      return 0;
    v = (part[0] << 24) | (part[1] << 16) | (part[2] << 8) | part[3];
    break;
  }
  if (addr)
    addr->s_addr = htonl(v);
  return 1;
}

/* Gives the address of a dotted text, or INADDR_NONE. */
in_addr_t inet_addr(const char *text) {
  struct in_addr a;
  return inet_aton(text, &a) ? a.s_addr : INADDR_NONE;
}

/* Gives an address as dotted text, in a static buffer. */
char *inet_ntoa(struct in_addr addr) {
  static char buf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &addr, buf, sizeof buf);
  return buf;
}

/* Writes an IPv6 address as text: the longest run of zero groups
   (two or more) becomes "::". */
static const char *ntop6(const unsigned char *a, char *buf, socklen_t size) {
  char tmp[INET6_ADDRSTRLEN];
  int best = -1, bestlen = 0, i, at = 0;
  for (i = 0; i < 8;) {
    int j = i;
    while (j < 8 && a[2 * j] == 0 && a[2 * j + 1] == 0)
      j++;
    if (j - i > bestlen && j - i >= 2) {
      best = i;
      bestlen = j - i;
    }
    i = j > i ? j : i + 1;
  }
  for (i = 0; i < 8; i++) {
    if (i == best) {
      at += sprintf(tmp + at, "::");
      i += bestlen - 1;
      continue;
    }
    if (i && !(best >= 0 && i == best + bestlen))
      tmp[at++] = ':';
    at += sprintf(tmp + at, "%x", (a[2 * i] << 8) | a[2 * i + 1]);
  }
  tmp[at] = 0;
  if ((socklen_t)at >= size) {
    errno = ENOSPC;
    return NULL;
  }
  strcpy(buf, tmp);
  return buf;
}

/* Writes an address as text. */
const char *inet_ntop(int family, const void *addr, char *buf, socklen_t size) {
  if (family == AF_INET) {
    const unsigned char *b = addr;
    char tmp[INET_ADDRSTRLEN];
    int n = sprintf(tmp, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if ((socklen_t)n >= size) {
      errno = ENOSPC;
      return NULL;
    }
    strcpy(buf, tmp);
    return buf;
  }
  if (family == AF_INET6)
    return ntop6(addr, buf, size);
  errno = EAFNOSUPPORT;
  return NULL;
}

/* Reads IPv6 text into 16 bytes. Gives 1 when the text is one. */
static int pton6(const char *text, unsigned char *out) {
  unsigned short g[8];
  int n = 0, gap = -1;
  const char *p = text;
  if (p[0] == ':' && p[1] == ':') {
    gap = 0;
    p += 2;
  }
  while (*p) {
    char *end;
    unsigned long v;
    if (n == 8)
      return 0;
    if (strchr(p, '.') && !strchr(p, ':')) {
      struct in_addr a;
      if (n > 6 || inet_pton(AF_INET, p, &a) != 1)
        return 0;
      g[n++] = (unsigned short)(ntohl(a.s_addr) >> 16);
      g[n++] = (unsigned short)ntohl(a.s_addr);
      break;
    }
    v = strtoul(p, &end, 16);
    if (end == p || end - p > 4)
      return 0;
    g[n++] = (unsigned short)v;
    p = end;
    if (*p == ':') {
      p++;
      if (*p == ':') {
        if (gap >= 0)
          return 0;
        gap = n;
        p++;
      } else if (!*p) {
        return 0;
      }
    } else if (*p) {
      return 0;
    }
  }
  if (gap < 0 && n != 8)
    return 0;
  memset(out, 0, 16);
  if (gap < 0)
    gap = n;
  {
    int i, tail = n - gap;
    for (i = 0; i < gap; i++) {
      out[2 * i] = (unsigned char)(g[i] >> 8);
      out[2 * i + 1] = (unsigned char)g[i];
    }
    for (i = 0; i < tail; i++) {
      out[16 - 2 * tail + 2 * i] = (unsigned char)(g[gap + i] >> 8);
      out[16 - 2 * tail + 2 * i + 1] = (unsigned char)g[gap + i];
    }
  }
  return 1;
}

/* Reads address text: four decimal numbers for AF_INET, as RFC 3493
   says, or IPv6 text. Gives 1, 0 for text that is not an address, or
   -1 with EAFNOSUPPORT. */
int inet_pton(int family, const char *text, void *addr) {
  if (family == AF_INET) {
    unsigned char b[4];
    int n = 0;
    const char *p = text;
    while (n < 4) {
      unsigned v = 0;
      int digits = 0;
      while (*p >= '0' && *p <= '9' && digits < 4) {
        v = v * 10 + (unsigned)(*p++ - '0');
        digits++;
      }
      if (!digits || v > 255 || (digits > 1 && p[-digits] == '0'))
        return 0;
      b[n++] = (unsigned char)v;
      if (n < 4 && *p++ != '.')
        return 0;
    }
    if (*p)
      return 0;
    memcpy(addr, b, 4);
    return 1;
  }
  if (family == AF_INET6)
    return pton6(text, addr);
  errno = EAFNOSUPPORT;
  return -1;
}

/* ------------------------------------------------------------------ */
/* The resolver of MacTCP and Open Transport */

#define OPENRESOLVER 1
#define CLOSERESOLVER 2
#define STRTOADDR 3
#define ADDRTONAME 6

#pragma options align=mac68k
struct host_info {
  long rtnCode;
  char cname[255];
  unsigned long addr[4];
};
#pragma options align=reset

/* A Pascal result procedure in 68k code that does nothing: it takes
   its two arguments off the stack. The caller looks at rtnCode.
     movea.l (sp)+,a0 ; addq.l #8,sp ; jmp (a0) */
static const unsigned short no_result[] = {0x205F, 0x508F, 0x4ED0};

static Handle dnr;      /* the resolver's code, locked */
static int dnr_state;   /* 0 not tried, 1 open, -1 not there */

/* Calls the resolver with a selector and up to four long arguments. */
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

/* Looks in one folder for the MacTCP control panel (type 'cdev', creator
   'ztcp' or 'mtcp') and opens its resource fork. Gives its reference
   number, or -1. */
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

/* Closes the resolver when the program ends. */
static void dnr_close(void) {
  if (dnr_state > 0)
    dnr_call(CLOSERESOLVER, 0, 0, 0, 0);
  dnr_state = -1;
}

/* Loads and opens the resolver the first time. Gives 0, or -1. */
static int dnr_open(void) {
  static const OSType folders[] = {kControlPanelFolderType, kExtensionFolderType, kSystemFolderType};
  short ref = -1, vol;
  long dir;
  unsigned i;
  if (dnr_state)
    return dnr_state > 0 ? 0 : -1;
  dnr_state = -1;
  if (__mac_ip_address() == 0)
    return -1;
  for (i = 0; ref == -1 && i < sizeof folders / sizeof folders[0]; i++)
    if (FindFolder(kOnSystemDisk, folders[i], 0, &vol, &dir) == noErr)
      ref = open_panel(vol, dir);
  /* Open Transport keeps the resolver in the System file. */
  dnr = ref != -1 ? Get1IndResource('dnrp', 1) : 0;
  if (!dnr)
    dnr = GetIndResource('dnrp', 1);
  if (dnr)
    DetachResource(dnr);
  if (ref != -1)
    CloseResFile(ref);
  if (!dnr)
    return -1;
  HLock(dnr);
  if (dnr_call(OPENRESOLVER, 0, 0, 0, 0) != noErr) {
    DisposeHandle(dnr);
    dnr = 0;
    return -1;
  }
  dnr_state = 1;
  atexit(dnr_close);
  return 0;
}

/* Waits for a resolver call: its rtnCode stays cacheFault (or
   inProgress) until it ends. */
static long dnr_wait(long rc, volatile struct host_info *hi) {
  if (rc != cacheFault)
    return rc;
  while (hi->rtnCode == cacheFault || hi->rtnCode == inProgress)
    __mac_wait();
  return hi->rtnCode;
}

/* Finds the addresses of a name with the resolver. Gives how many (up
   to max), or 0 with h_errno set. */
static int dnr_lookup(const char *name, in_addr_t *out, int max, char *cname, size_t cnamelen) {
  static struct host_info hi;
  long rc;
  int n = 0;
  if (dnr_open() < 0) {
    h_errno = NO_RECOVERY;
    return 0;
  }
  memset(&hi, 0, sizeof hi);
  hi.rtnCode = cacheFault;
  rc = dnr_call(STRTOADDR, (long)name, (long)&hi, (long)no_result, 0);
  rc = dnr_wait(rc, &hi);
  if (rc != noErr) {
    h_errno = rc == noNameServer || rc == noAnsErr ? TRY_AGAIN : HOST_NOT_FOUND;
    return 0;
  }
  while (n < max && n < 4 && hi.addr[n])
    out[n] = hi.addr[n], n++;
  if (cname && cnamelen) {
    strncpy(cname, hi.cname, cnamelen - 1);
    cname[cnamelen - 1] = 0;
    /* The resolver ends a name with a dot. */
    if (*cname && cname[strlen(cname) - 1] == '.')
      cname[strlen(cname) - 1] = 0;
  }
  if (!n)
    h_errno = NO_DATA;
  return n;
}

/* Finds the name of an address with the resolver. Gives 0, or -1. */
static int dnr_name(in_addr_t addr, char *name, size_t len) {
  static struct host_info hi;
  long rc;
  if (dnr_open() < 0)
    return -1;
  memset(&hi, 0, sizeof hi);
  hi.rtnCode = cacheFault;
  rc = dnr_call(ADDRTONAME, (long)addr, (long)&hi, (long)no_result, 0);
  if (dnr_wait(rc, &hi) != noErr || !hi.cname[0])
    return -1;
  strncpy(name, hi.cname, len - 1);
  name[len - 1] = 0;
  if (name[strlen(name) - 1] == '.')
    name[strlen(name) - 1] = 0;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Host names */

/* Finds the IPv4 addresses of a host name. Gives how many (up to max),
   or 0 with h_errno set. */
static int lookup(const char *name, in_addr_t *out, int max, char *cname, size_t cnamelen) {
  struct in_addr a;
  char me[64];
  if (cname && cnamelen) {
    strncpy(cname, name, cnamelen - 1);
    cname[cnamelen - 1] = 0;
  }
  if (inet_aton(name, &a)) {
    out[0] = a.s_addr;
    return 1;
  }
  if (strcasecmp(name, "localhost") == 0 || strcasecmp(name, "localhost.localdomain") == 0) {
    out[0] = htonl(INADDR_LOOPBACK);
    return 1;
  }
  if (gethostname(me, sizeof me) == 0 && strcasecmp(name, me) == 0) {
    in_addr_t ip = __mac_ip_address();
    out[0] = ip ? ip : htonl(INADDR_LOOPBACK);
    return 1;
  }
  return dnr_lookup(name, out, max, cname, cnamelen);
}

static struct {
  struct hostent h;
  char name[256];
  char *aliases[1];
  char *list[5];
  in_addr_t addr[4];
} host;

/* Finds a host by name, into a static hostent. */
struct hostent *gethostbyname(const char *name) {
  int n, i;
  if (!name) {
    h_errno = HOST_NOT_FOUND;
    return NULL;
  }
  n = lookup(name, host.addr, 4, host.name, sizeof host.name);
  if (!n)
    return NULL;
  host.h.h_name = host.name;
  host.aliases[0] = NULL;
  host.h.h_aliases = host.aliases;
  host.h.h_addrtype = AF_INET;
  host.h.h_length = 4;
  for (i = 0; i < n; i++)
    host.list[i] = (char *)&host.addr[i];
  host.list[n] = NULL;
  host.h.h_addr_list = host.list;
  return &host.h;
}

/* Finds a host by address, into a static hostent. The name is the
   resolver's, or the address as text. */
struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type) {
  in_addr_t a;
  if (type != AF_INET || len < 4) {
    h_errno = HOST_NOT_FOUND;
    return NULL;
  }
  memcpy(&a, addr, 4);
  if (a == htonl(INADDR_LOOPBACK))
    strcpy(host.name, "localhost");
  else if (dnr_name(a, host.name, sizeof host.name) < 0)
    inet_ntop(AF_INET, &a, host.name, sizeof host.name);
  host.addr[0] = a;
  host.list[0] = (char *)&host.addr[0];
  host.list[1] = NULL;
  host.aliases[0] = NULL;
  host.h.h_name = host.name;
  host.h.h_aliases = host.aliases;
  host.h.h_addrtype = AF_INET;
  host.h.h_length = 4;
  host.h.h_addr_list = host.list;
  return &host.h;
}

/* The text for an h_errno value. */
const char *hstrerror(int err) {
  switch (err) {
  case HOST_NOT_FOUND: return "Unknown host";
  case TRY_AGAIN: return "Host name lookup failure";
  case NO_RECOVERY: return "Unknown server error";
  case NO_DATA: return "No address associated with name";
  default: return "Unknown resolver error";
  }
}

/* ------------------------------------------------------------------ */
/* Services and protocols */

static const struct {
  const char *name;
  int port;
  int udp;  /* 1 for UDP only, 0 for TCP and UDP, -1 for TCP only */
} services[] = {
    {"echo", 7, 0},     {"discard", 9, 0},  {"daytime", 13, 0}, {"ftp-data", 20, -1},
    {"ftp", 21, -1},    {"ssh", 22, -1},    {"telnet", 23, -1}, {"smtp", 25, -1},
    {"time", 37, 0},    {"domain", 53, 0},  {"gopher", 70, -1}, {"finger", 79, -1},
    {"http", 80, -1},   {"pop3", 110, -1},  {"nntp", 119, -1},  {"ntp", 123, 1},
    {"imap", 143, -1},  {"snmp", 161, 1},   {"irc", 194, -1},   {"https", 443, -1},
    {"imaps", 993, -1}, {"pop3s", 995, -1},
};

/* Tells if a service entry is for the protocol name proto (NULL: any). */
static int proto_fits(int udp, const char *proto) {
  if (!proto)
    return 1;
  if (strcmp(proto, "udp") == 0)
    return udp >= 0;
  if (strcmp(proto, "tcp") == 0)
    return udp <= 0;
  return 0;
}

static struct servent serv;
static char *serv_aliases[1];

/* Fills the static servent for entry i. */
static struct servent *serv_entry(int i, const char *proto) {
  serv.s_name = (char *)services[i].name;
  serv_aliases[0] = NULL;
  serv.s_aliases = serv_aliases;
  serv.s_port = htons((unsigned short)services[i].port);
  serv.s_proto = (char *)(proto ? proto : services[i].udp > 0 ? "udp" : "tcp");
  return &serv;
}

/* Finds a well-known service by name. */
struct servent *getservbyname(const char *name, const char *proto) {
  unsigned i;
  for (i = 0; i < sizeof services / sizeof services[0]; i++)
    if (strcmp(services[i].name, name) == 0 && proto_fits(services[i].udp, proto))
      return serv_entry((int)i, proto);
  return NULL;
}

/* Finds a well-known service by port (in network order). */
struct servent *getservbyport(int port, const char *proto) {
  unsigned i;
  for (i = 0; i < sizeof services / sizeof services[0]; i++)
    if (htons((unsigned short)services[i].port) == (unsigned short)port && proto_fits(services[i].udp, proto))
      return serv_entry((int)i, proto);
  return NULL;
}

static const struct {
  const char *name;
  int number;
} protocols[] = {{"ip", 0}, {"icmp", 1}, {"tcp", 6}, {"udp", 17}};

static struct protoent proto_ent;
static char *proto_aliases[1];

/* Fills the static protoent for entry i. */
static struct protoent *proto_entry(int i) {
  proto_ent.p_name = (char *)protocols[i].name;
  proto_aliases[0] = NULL;
  proto_ent.p_aliases = proto_aliases;
  proto_ent.p_proto = protocols[i].number;
  return &proto_ent;
}

/* Finds a protocol by name. */
struct protoent *getprotobyname(const char *name) {
  unsigned i;
  for (i = 0; i < sizeof protocols / sizeof protocols[0]; i++)
    if (strcmp(protocols[i].name, name) == 0)
      return proto_entry((int)i);
  return NULL;
}

/* Finds a protocol by number. */
struct protoent *getprotobynumber(int number) {
  unsigned i;
  for (i = 0; i < sizeof protocols / sizeof protocols[0]; i++)
    if (protocols[i].number == number)
      return proto_entry((int)i);
  return NULL;
}

/* ------------------------------------------------------------------ */
/* getaddrinfo and getnameinfo */

/* Gives the port of a service (a number, or a name), in network order,
   or -1. */
static int service_port(const char *service, int socktype, int numeric) {
  char *end;
  unsigned long v;
  struct servent *s;
  if (!service || !*service)
    return 0;
  v = strtoul(service, &end, 10);
  if (!*end && v <= 65535)
    return htons((unsigned short)v);
  if (numeric)
    return -1;
  s = getservbyname(service, socktype == SOCK_DGRAM ? "udp" : socktype == SOCK_STREAM ? "tcp" : NULL);
  return s ? s->s_port : -1;
}

/* Finds the addresses of a host and a service. Only IPv4 addresses
   come back. */
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
  in_addr_t addr[4];
  char cname[256];
  int n, i, t, port, flags = hints ? hints->ai_flags : 0;
  int family = hints ? hints->ai_family : AF_UNSPEC;
  int socktype = hints ? hints->ai_socktype : 0;
  int protocol = hints ? hints->ai_protocol : 0;
  struct addrinfo *head = NULL, **tail = &head;
  *res = NULL;
  if (!node && !service)
    return EAI_NONAME;
  if (family != AF_UNSPEC && family != AF_INET)
    return family == AF_INET6 ? EAI_NONAME : EAI_FAMILY;
  if (socktype && socktype != SOCK_STREAM && socktype != SOCK_DGRAM)
    return EAI_SOCKTYPE;
  port = service_port(service, socktype, flags & AI_NUMERICSERV);
  if (port < 0)
    return EAI_SERVICE;
  if (!node) {
    addr[0] = (flags & AI_PASSIVE) ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    n = 1;
    strcpy(cname, "localhost");
  } else if (flags & AI_NUMERICHOST) {
    struct in_addr a;
    if (inet_pton(AF_INET, node, &a) != 1)
      return EAI_NONAME;
    addr[0] = a.s_addr;
    n = 1;
    strncpy(cname, node, sizeof cname - 1);
    cname[sizeof cname - 1] = 0;
  } else {
    n = lookup(node, addr, 4, cname, sizeof cname);
    if (!n)
      return h_errno == TRY_AGAIN ? EAI_AGAIN : h_errno == NO_RECOVERY ? EAI_FAIL : EAI_NONAME;
  }
  for (i = 0; i < n; i++) {
    for (t = 0; t < 2; t++) {
      int type = t == 0 ? SOCK_STREAM : SOCK_DGRAM;
      struct {
        struct addrinfo ai;
        struct sockaddr_in in;
      } *e;
      if (socktype && socktype != type)
        continue;
      if (protocol && protocol != (type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP))
        continue;
      e = calloc(1, sizeof *e);
      if (!e) {
        freeaddrinfo(head);
        return EAI_MEMORY;
      }
      e->ai.ai_family = AF_INET;
      e->ai.ai_socktype = type;
      e->ai.ai_protocol = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
      e->ai.ai_addrlen = sizeof e->in;
      e->ai.ai_addr = (struct sockaddr *)&e->in;
      e->in.sin_family = AF_INET;
      e->in.sin_port = (in_port_t)port;
      e->in.sin_addr.s_addr = addr[i];
      if (!head && (flags & AI_CANONNAME))
        e->ai.ai_canonname = strdup(cname);
      *tail = &e->ai;
      tail = &e->ai.ai_next;
    }
  }
  *res = head;
  return head ? 0 : EAI_NONAME;
}

/* Frees what getaddrinfo gave. */
void freeaddrinfo(struct addrinfo *ai) {
  while (ai) {
    struct addrinfo *next = ai->ai_next;
    free(ai->ai_canonname);
    free(ai);
    ai = next;
  }
}

/* The text for a getaddrinfo error. */
const char *gai_strerror(int err) {
  switch (err) {
  case 0: return "Success";
  case EAI_BADFLAGS: return "Bad value for ai_flags";
  case EAI_NONAME: return "Name or service not known";
  case EAI_AGAIN: return "Temporary failure in name resolution";
  case EAI_FAIL: return "Non-recoverable failure in name resolution";
  case EAI_FAMILY: return "ai_family not supported";
  case EAI_SOCKTYPE: return "ai_socktype not supported";
  case EAI_SERVICE: return "Servname not supported for ai_socktype";
  case EAI_MEMORY: return "Memory allocation failure";
  case EAI_SYSTEM: return "System error";
  case EAI_OVERFLOW: return "Argument buffer overflow";
  default: return "Unknown error";
  }
}

/* Gives the host and service names of an address. */
int getnameinfo(const struct sockaddr *sa, socklen_t len, char *host, socklen_t hostlen, char *serv,
                socklen_t servlen, int flags) {
  const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
  if (!sa || len < sizeof *in || sa->sa_family != AF_INET)
    return EAI_FAMILY;
  if (host && hostlen) {
    char name[256];
    int found = 0;
    if (!(flags & NI_NUMERICHOST)) {
      if (in->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
        strcpy(name, "localhost");
        found = 1;
      } else {
        found = dnr_name(in->sin_addr.s_addr, name, sizeof name) == 0;
      }
    }
    if (!found) {
      if (flags & NI_NAMEREQD)
        return EAI_NONAME;
      inet_ntop(AF_INET, &in->sin_addr, name, sizeof name);
    }
    if (strlen(name) >= hostlen)
      return EAI_OVERFLOW;
    strcpy(host, name);
  }
  if (serv && servlen) {
    char text[NI_MAXSERV];
    struct servent *s = (flags & NI_NUMERICSERV) ? NULL : getservbyport(in->sin_port, flags & NI_DGRAM ? "udp" : "tcp");
    if (s)
      strncpy(text, s->s_name, sizeof text - 1), text[sizeof text - 1] = 0;
    else
      sprintf(text, "%u", (unsigned)ntohs(in->sin_port));
    if (strlen(text) >= servlen)
      return EAI_OVERFLOW;
    strcpy(serv, text);
  }
  return 0;
}
