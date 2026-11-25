/* Checks the pipes and sockets of the C library: pipe and poll in one
 * program (runtime/libc/pipe.c), TCP and UDP over MacTCP and the name
 * calls (socket.c, netdb.c), and AF_UNIX sockets and named pipes over
 * the PPC Toolbox (unix.c). tests/run.sh runs it in tools/sim68k.py --os
 * and tools/simppc.py, whose fake MacTCP uses the host's loopback
 * network, and checks the console output: one "ok" line per check, then
 * "all ok".
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int failures;

/* Prints one check's result, and counts the failures. */
static void check(const char *what, int ok) {
  printf("%s %s\n", ok ? "ok" : "FAILED", what);
  if (!ok)
    failures++;
}

/* A pipe in one program: data, a nonblocking read, poll, the end. */
static void check_pipe(void) {
  int p[2];
  char buf[16];
  struct pollfd pf;
  struct stat st;
  check("pipe", pipe(p) == 0 && p[0] >= 3 && p[1] > p[0]);
  check("pipe write", write(p[1], "hello", 5) == 5);
  pf.fd = p[0];
  pf.events = POLLIN;
  check("pipe poll", poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN));
  check("pipe read", read(p[0], buf, sizeof buf) == 5 && memcmp(buf, "hello", 5) == 0);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  check("pipe empty", read(p[0], buf, sizeof buf) == -1 && errno == EAGAIN);
  check("pipe fstat", fstat(p[0], &st) == 0 && S_ISFIFO(st.st_mode));
  close(p[1]);
  check("pipe end", read(p[0], buf, sizeof buf) == 0);
  close(p[0]);
}

/* Address text and names. */
static void check_names(void) {
  struct in_addr a;
  unsigned char six[16];
  char text[64];
  struct addrinfo hints, *res = NULL;
  struct hostent *h;
  check("inet_pton", inet_pton(AF_INET, "10.1.2.3", &a) == 1 && ntohl(a.s_addr) == 0x0A010203);
  check("inet_pton bad", inet_pton(AF_INET, "10.1.2", &a) == 0);
  check("inet_ntop", strcmp(inet_ntop(AF_INET, &a, text, sizeof text), "10.1.2.3") == 0);
  check("inet_addr", inet_addr("127.0.0.1") == htonl(INADDR_LOOPBACK));
  check("inet6", inet_pton(AF_INET6, "fe80::1", six) == 1 &&
                     strcmp(inet_ntop(AF_INET6, six, text, sizeof text), "fe80::1") == 0);
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  check("getaddrinfo", getaddrinfo("localhost", "http", &hints, &res) == 0 && res &&
                           ((struct sockaddr_in *)res->ai_addr)->sin_port == htons(80) &&
                           ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
  freeaddrinfo(res);
  h = gethostbyname("127.0.0.1");
  check("gethostbyname", h && h->h_length == 4 && memcmp(h->h_addr, "\x7f\0\0\1", 4) == 0);
  check("getservbyname", getservbyname("ftp", "tcp") && getservbyname("ftp", "tcp")->s_port == htons(21));
}

/* TCP over the loopback network: a listener, a client, data both ways,
   shutdown and the end. */
static void check_tcp(void) {
  struct sockaddr_in sa;
  socklen_t len = sizeof sa;
  char buf[64];
  int l, c, s;
  fd_set rd;
  struct timeval tv = {2, 0};
  l = socket(AF_INET, SOCK_STREAM, 0);
  check("tcp socket", l >= 3);
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  check("tcp bind", bind(l, (struct sockaddr *)&sa, sizeof sa) == 0);
  check("tcp listen", listen(l, 2) == 0);
  check("tcp getsockname", getsockname(l, (struct sockaddr *)&sa, &len) == 0 && sa.sin_port != 0);
  c = socket(AF_INET, SOCK_STREAM, 0);
  check("tcp connect", connect(c, (struct sockaddr *)&sa, sizeof sa) == 0);
  FD_ZERO(&rd);
  FD_SET(l, &rd);
  check("tcp select listener", select(l + 1, &rd, NULL, NULL, &tv) == 1 && FD_ISSET(l, &rd));
  s = accept(l, NULL, NULL);
  check("tcp accept", s >= 3);
  check("tcp send", send(c, "ping", 4, 0) == 4);
  check("tcp recv", recv(s, buf, 4, MSG_WAITALL) == 4 && memcmp(buf, "ping", 4) == 0);
  check("tcp write", write(s, "pong!", 5) == 5);
  check("tcp read", read(c, buf, sizeof buf) == 5 && memcmp(buf, "pong!", 5) == 0);
  check("tcp shutdown", shutdown(c, SHUT_WR) == 0);
  check("tcp end", recv(s, buf, sizeof buf, 0) == 0);
  len = sizeof sa;
  check("tcp getpeername", getpeername(s, (struct sockaddr *)&sa, &len) == 0 &&
                               sa.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
  close(s);
  close(c);
  close(l);
}

/* UDP over the loopback network. */
static void check_udp(void) {
  struct sockaddr_in a, from;
  socklen_t len = sizeof a, flen = sizeof from;
  char buf[32];
  int x = socket(AF_INET, SOCK_DGRAM, 0), y = socket(AF_INET, SOCK_DGRAM, 0);
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  check("udp bind", bind(x, (struct sockaddr *)&a, sizeof a) == 0);
  check("udp port", getsockname(x, (struct sockaddr *)&a, &len) == 0 && a.sin_port != 0);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  check("udp sendto", sendto(y, "datagram", 8, 0, (struct sockaddr *)&a, sizeof a) == 8);
  check("udp recvfrom", recvfrom(x, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen) == 8 &&
                            memcmp(buf, "datagram", 8) == 0 && from.sin_port != 0);
  close(x);
  close(y);
}

/* An AF_UNIX connection between two sockets of this program, through
   the PPC Toolbox. */
static void check_unix(void) {
  struct sockaddr_un un;
  char buf[32];
  int l = socket(AF_UNIX, SOCK_STREAM, 0), c, s;
  memset(&un, 0, sizeof un);
  un.sun_family = AF_UNIX;
  strcpy(un.sun_path, "/tmp/test.sock");
  check("unix bind", bind(l, (struct sockaddr *)&un, sizeof un) == 0);
  check("unix listen", listen(l, 1) == 0);
  c = socket(AF_UNIX, SOCK_STREAM, 0);
  check("unix connect", connect(c, (struct sockaddr *)&un, sizeof un) == 0);
  s = accept(l, NULL, NULL);
  check("unix accept", s >= 3);
  /* A PPC write ends when the other end reads it, so the reader must be
     waiting: poll starts its read. */
  {
    struct pollfd pf = {s, POLLIN, 0};
    poll(&pf, 1, 0);
  }
  check("unix send", send(c, "over ppc", 8, 0) == 8);
  check("unix recv", recv(s, buf, sizeof buf, 0) == 8 && memcmp(buf, "over ppc", 8) == 0);
  {
    struct pollfd pf = {s, POLLIN, 0};
    poll(&pf, 1, 0);
  }
  check("unix shutdown", shutdown(c, SHUT_WR) == 0);
  check("unix end", recv(s, buf, sizeof buf, 0) == 0);
  close(s);
  close(c);
  close(l);
}

/* Writes to a named pipe from a thread. */
static void *fifo_writer(void *arg) {
  int fd = open((const char *)arg, O_WRONLY);
  if (fd >= 0) {
    write(fd, "through the fifo", 16);
    close(fd);
  }
  return (void *)(long)fd;
}

/* A named pipe between two threads (as it would be between two
   programs). */
static void check_fifo(void) {
  pthread_t t;
  struct stat st;
  char buf[32];
  long got = 0, n;
  int fd;
  void *res;
  unlink("pipe");
  check("mkfifo", mkfifo("pipe", 0600) == 0);
  check("fifo stat", stat("pipe", &st) == 0 && S_ISFIFO(st.st_mode));
  pthread_create(&t, NULL, fifo_writer, (void *)"pipe");
  fd = open("pipe", O_RDONLY);
  check("fifo open", fd >= 3);
  while ((n = read(fd, buf + got, sizeof buf - (size_t)got)) > 0)
    got += n;
  check("fifo read", got == 16 && memcmp(buf, "through the fifo", 16) == 0);
  pthread_join(t, &res);
  check("fifo writer", (long)res >= 3);
  close(fd);
  unlink("pipe");
}

int main(void) {
  check_pipe();
  check_names();
  check_tcp();
  check_udp();
  check_unix();
  check_fifo();
  printf(failures ? "FAILED\n" : "all ok\n");
  return failures != 0;
}
