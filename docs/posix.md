# POSIX functions

The C library has POSIX functions for programs that come from Unix,
such as the portable layer of MFC. Some functions work as on Unix. The
others fail with the usual error, because the classic Mac OS does not
have the function. A program can find the failure and continue.

## Functions that work

| Functions | Behavior |
| --- | --- |
| `dup`, `dup2`, `fcntl` | Two descriptors share one open file and its mark. The file closes when its last descriptor closes. `fcntl` does `F_DUPFD`, `F_GETFD`, `F_SETFD`, `F_GETFL` and `F_SETFL` (`O_APPEND`, `O_NONBLOCK`). |
| `open` | A path that ends in `/..namedfork/rsrc` opens the resource fork of the file, as on Mac OS X. `/..namedfork/data` opens the data fork. |
| `pread`, `pwrite` | They read and write at an offset. The mark does not change. |
| `fsync` | It writes the buffers of the file to the disk (`PBFlushFile`). |
| `access` | It tells if the item exists. `W_OK` fails with `EACCES` for a locked file. |
| `chmod` | It locks a file when the mode has no write permission, and unlocks it otherwise. |
| `futimens` | It sets the modification date of an open file. `UTIME_NOW` and `UTIME_OMIT` work. |
| `chdir` | It sets the current folder (`PBHSetVol`). |
| `realpath` | It gives the absolute POSIX path, without `.` and `..`. The item must exist. |
| `readlink` | There are no symbolic links: it fails with `EINVAL`. |
| `mmap`, `munmap`, `msync`, `mprotect` | See [Memory mappings](#memory-mappings). |
| `shm_open`, `shm_unlink` | A shared memory object is a file in the Temporary Items folder (in the current folder before System 7). |
| `poll`, `select` | Files and the console are always ready. Pipes and sockets are ready when they have data or space. When no descriptor is ready, it waits for the time-out. Other threads and other programs run while it waits. |
| `pipe`, `pipe2` | A pipe in one program, with a buffer of 4096 bytes. See [Networking and pipes](network.md). |
| `readv`, `writev` | They read and write each buffer in sequence. |
| `ioctl` | `FIONREAD` gives the number of bytes after the mark of a file, or the bytes that a pipe or socket can read now. `FIONBIO` sets `O_NONBLOCK`. `SIOCATMARK` gives 0. Other requests fail with `ENOTTY`. |
| `uname`, `gethostname` | `MacOS`, `Macintosh`, the System version from Gestalt, and `m68k` or `powerpc`. |
| `getuid`, `geteuid`, `getgid`, `getegid` | 0. |
| `chown`, `fchown` | They succeed when the item exists: there is one user. |
| `fchmod` | The same as `chmod`, for an open file. |
| `getgroups` | No other groups (0). |
| `getsid` | The program is the only process of its session. |
| `pause` | No signal comes from outside the program, so it waits for ever. Other threads continue to run. |
| `sysconf` | One processor (`_SC_NPROCESSORS_ONLN` is 84, as on Linux). |
| `flock` | It always succeeds: no other program uses the lock. |
| `sigaction` | It uses `signal`. Only `sa_handler` is used. |
| `malloc_usable_size` | The size of the block. |
| `mkdtemp` | It makes a new folder with a name from the template. The last six characters of the template must be `XXXXXX`. |
| `dlopen`, `dlsym`, `dlclose`, `dlerror`, `dladdr` | `dlopen(NULL)` gives a handle for the program. `dlsym` finds no symbol, and all other `dlopen` calls fail. |
| `clock_gettime`, `clock_getres` | `CLOCK_MONOTONIC` and `CLOCK_REALTIME`. See [Clocks](#clocks). |
| `getentropy` | It gives up to 256 random bytes. See [Random bytes](#random-bytes). |

## Functions that fail

| Functions | Error |
| --- | --- |
| `fork`, `execve` and the other `exec` functions | `ENOSYS` |
| `waitpid` | `ECHILD` |
| `posix_spawn`, `posix_spawnp` | `ENOSYS` (the file action functions succeed and keep nothing) |
| `tcgetattr`, `tcsetattr`, `tcdrain`, `tcflow`, `tcflush` | `ENOTTY` |

`cfmakeraw` and the `cfget*speed` and `cfset*speed` functions change
only the `struct termios`.

## Clocks

The C library has a fine clock. It uses the first of these that the
Mac has:

| Clock | Step | Macs |
| --- | --- | --- |
| `UpTime` (DriverServicesLib) | 1 microsecond | PowerPC, Mac OS 7.5.2 and later (PCI Power Macs). |
| `Microseconds` | 1 microsecond | 68k with System 7 and later, all PowerPC Macs. |
| `TickCount` | 1/60 second | 68k with System 6. |

The C library looks for DriverServicesLib with `GetSharedLibrary`. It
does not import the library. Thus the program also starts on a Mac
without the library.

- `CLOCK_MONOTONIC` is the fine clock: the time since the Mac started.
  It never goes back. `std::chrono::steady_clock` uses it.
- `CLOCK_REALTIME`, `gettimeofday` and `time` are the Mac's clock
  (`GetDateTime`) in UTC. The fine clock gives the fractions of a
  second.
- `clock_getres` gives 1 microsecond, or 1/60 second on System 6.
- `nanosleep`, `usleep` and `sleep` wait on the fine clock. While they
  wait, the other threads run. A wait that is shorter than the time
  that another thread runs takes longer than its time.
- `time.h` defines `CLOCK_MONOTONIC` only with `_GNU_SOURCE`. Without
  it, define `CLOCK_MONOTONIC` as 4.

## Random bytes

`getentropy` fills a buffer with random bytes. picolibc's `arc4random`
and libstdc++'s `std::random_device` use it.

The classic Mac OS has no random device. Thus `getentropy` collects
samples and mixes them into a pool of 32 bytes with SHA-256:

- the jitter of the fine clock against a counted loop. Interrupts (the
  vertical blanking task, sound, ADB, the disk and the network) change
  the count.
- the fine clock, the tick count and the date and time.
- the position of the mouse, the mouse button and the keyboard map.
- the addresses of the stack, the program and the heap.

Each call mixes new samples into the pool. Then it makes the output
from the pool with SHA-256, and replaces the pool. Thus the output does
not show the pool or the output of an earlier call.

- One call gives a maximum of 256 bytes. For more, `getentropy` fails
  with `EIO`.
- The first call takes more samples than the subsequent calls. On a
  68000 it takes approximately 140,000 instructions (approximately 0.2
  second on a Mac Plus). A subsequent call takes approximately 55,000.
  Use `arc4random` for many random numbers: it calls `getentropy` only
  to get a new key.
- On System 6, the fine clock is the tick count. Each jitter sample
  then waits for one tick, and the first call takes approximately 0.2
  second.
- The samples are weaker than the random device of a modern system. Do
  not use them for long-term keys. Mouse and keyboard activity before
  the call adds entropy.
- A program can define its own `getentropy`. Then the linker does not
  use the one in the C library.

## Memory mappings

The classic Mac OS has no memory mapping calls for a program. Thus a
mapping is a block in the heap of the application:

- An anonymous mapping (`MAP_ANONYMOUS`) is a block of zeros.
- A file mapping is a block with a copy of the bytes of the file.
  `msync` and `munmap` write the block of a `MAP_SHARED` mapping with
  `PROT_WRITE` back to the file. The file does not become longer.
- Two `MAP_SHARED` mappings of the same bytes of a file share one block.
- `MAP_FIXED` with `MAP_ANONYMOUS` in an existing mapping sets those
  bytes to zero. Other `MAP_FIXED` mappings fail with `ENOMEM`.
- `mprotect` changes nothing. The memory is always readable and
  writable.
- `munmap` of a part of a mapping does nothing.
- A mapping uses memory of the application partition. Give the
  application a sufficiently large partition for its mappings.

## Headers

The C library adds `dlfcn.h`, `sys/utsname.h`, `sys/uio.h` and the
socket headers (`sys/socket.h`, `sys/un.h`, `netinet/in.h`,
`netinet/tcp.h`, `arpa/inet.h` and `netdb.h`). Its `sys/ioctl.h` has
`FIONREAD`, `FIONBIO` and `SIOCATMARK`. picolibc does not declare
`shm_open`, `shm_unlink`, `cfmakeraw` or `_SC_NPROCESSORS_ONLN`. Declare
them in the program:

```c
int shm_open(const char *name, int flags, mode_t mode);
int shm_unlink(const char *name);
void cfmakeraw(struct termios *t);
#define _SC_NPROCESSORS_ONLN 84
```

## Tests

`tests/libc.c` checks these functions. `tests/run.sh` runs it in
`tools/sim68k.py` and `tools/simppc.py`.
