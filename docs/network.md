# Networking and pipes

The C library has BSD sockets and pipes for 68k and PowerPC. Programs
from Unix can use them without changes. The code is in
`runtime/libc/socket.c`, `netdb.c`, `unix.c` and `pipe.c`.

## Summary

| Item | Uses | Between |
| --- | --- | --- |
| `pipe` | A buffer in the heap | Two parts (or threads) of one program |
| `AF_INET`, `SOCK_STREAM` | MacTCP TCP streams | Programs on the network |
| `AF_INET`, `SOCK_DGRAM` | MacTCP UDP streams | Programs on the network |
| `AF_UNIX`, `SOCK_STREAM` | PPC Toolbox sessions | Programs on this computer |
| `mkfifo` and `open` | PPC Toolbox sessions | Programs on this computer |

## Requirements

- TCP and UDP need MacTCP, or Open Transport with its MacTCP driver
  (System 7.5 to Mac OS 9). The library opens the `.IPP` driver at the
  first socket call. If the driver does not open, `socket` fails with
  `ENETDOWN`.
- `AF_UNIX` sockets and named pipes need the PPC Toolbox (System 7 and
  later). The library examines `Gestalt('ppc ')`. Without the PPC
  Toolbox, the calls fail with `EAFNOSUPPORT`.
- Names need the MacTCP resolver (`dnrp` resource of MacTCP or of the
  TCP/IP control panel). Without it, only numbers, `localhost` and the
  name of the computer resolve.

## Sockets

These functions work: `socket`, `bind`, `listen`, `accept`, `accept4`,
`connect`, `send`, `sendto`, `sendmsg`, `recv`, `recvfrom`, `recvmsg`,
`shutdown`, `getsockopt`, `setsockopt`, `getsockname`, `getpeername`,
`read`, `write`, `close`, `poll`, `select`, `ioctl` and `fcntl`.

- A TCP socket is one MacTCP stream. A listening socket keeps up to 4
  streams that wait for connections (passive opens). `listen` uses the
  smaller of the backlog and 4.
- A TCP send sends at most 32 KB with one `TCPSend` call. `send`
  continues until it sends all bytes, unless the socket is
  nonblocking.
- `MSG_PEEK`, `MSG_WAITALL`, `MSG_DONTWAIT` and `MSG_NOSIGNAL` work.
  There is no `SIGPIPE`.
- `O_NONBLOCK` and `FIONBIO` work. A nonblocking `connect` gives
  `EINPROGRESS`. Then `poll` gives `POLLOUT` when the connection opens.
- `SO_ERROR`, `SO_TYPE`, `SO_ACCEPTCONN`, `SO_REUSEADDR`,
  `SO_KEEPALIVE`, `SO_BROADCAST` and `SO_RCVTIMEO` work. The library
  accepts and ignores `SO_RCVBUF`, `SO_SNDBUF`, `SO_LINGER`,
  `SO_OOBINLINE`, `SO_SNDTIMEO`, `TCP_NODELAY`, `IP_TOS` and `IP_TTL`.
  Other options fail with `ENOPROTOOPT`.
- IPv6 addresses fail with `EAFNOSUPPORT`. MacTCP has only IPv4.
- `socketpair` fails with `EOPNOTSUPP`.

While a call waits, other threads run, and once a tick the library calls
`EventAvail` so that other programs run too.

## Names

`getaddrinfo`, `freeaddrinfo`, `gai_strerror`, `getnameinfo`,
`gethostbyname`, `gethostbyaddr`, `gethostname`, `getservbyname`,
`getservbyport`, `getprotobyname`, `getprotobynumber`, `inet_addr`,
`inet_aton`, `inet_ntoa`, `inet_pton` and `inet_ntop` work.
`inet_pton` and `inet_ntop` also do IPv6 text. `getaddrinfo` gives
only IPv4 addresses. The services are in a table in the library (for
example `http` is 80 and `ftp` is 21). There is no `/etc/services`.

## AF_UNIX sockets

An `AF_UNIX` socket is a PPC Toolbox port. The port name is the last 32
characters of the path, and the port type is `POSIX socket`. No file is
made. Two programs that use the same path connect.

- `bind` opens the port (`PPCOpen`). `listen` waits for a session
  (`PPCInform`). `connect` opens a port with no name and starts a
  session (`PPCStart`).
- The data goes in PPC blocks. `shutdown(SHUT_WR)` sends an empty block,
  and the other end then reads 0 bytes.
- A PPC write ends only when the other end reads. A program that sends
  and then receives on the same thread must first let the reader start:
  `poll` on the reading socket starts its read.

## Named pipes

`mkfifo` makes a file of type `fifo` (creator `ttxt`). `stat` gives
`S_IFIFO` for it. When a program opens this file with `open`, the
library opens a PPC Toolbox session with the name of the file:

- `O_RDONLY` waits for a writer.
- `O_WRONLY` connects to the reader.
- When the writer closes, the reader reads 0 bytes.

## Pipes

`pipe` and `pipe2` make a pipe in one program. The pipe has a buffer of
4096 bytes. A read waits for data, and a write waits for space, while
other threads run. When all write ends close, a read gives 0. When all
read ends close, a write fails with `EPIPE`. `fstat` gives `S_IFIFO`.

There are no processes, thus a pipe does not connect two programs. Use
a named pipe or an `AF_UNIX` socket for that.

## Tests

`tests/net.c` checks pipes, names, TCP, UDP, `AF_UNIX` sockets and
named pipes. `tests/run.sh` runs it in `tools/sim68k.py --os` and
`tools/simppc.py`. Their fake MacTCP uses the network of the host
(loopback), and their fake PPC Toolbox connects ports in the
simulator.
