# POSIX functions

The C library has POSIX functions for programs that come from Unix,
such as the portable layer of MFC. Some functions work as on Unix. The
others fail with the usual error, because the classic Mac OS does not
have the function. A program can find the failure and continue.

## Functions that work

| Functions | Behavior |
| --- | --- |
| `dup`, `dup2`, `fcntl` | Two descriptors share one open file and its mark. The file closes when its last descriptor closes. `fcntl` does `F_DUPFD`, `F_GETFD`, `F_SETFD`, `F_GETFL` and `F_SETFL` (`O_APPEND`, `O_NONBLOCK`). |
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
| `poll` | Files and the console are always ready. When no descriptor is ready, it waits for the time-out, and other threads run. |
| `ioctl` | `FIONREAD` gives the number of bytes after the mark of a file. Other requests fail with `ENOTTY`. |
| `uname`, `gethostname` | `MacOS`, `Macintosh`, the System version from Gestalt, and `m68k` or `powerpc`. |
| `getuid`, `geteuid`, `getgid`, `getegid` | 0. |
| `sysconf` | One processor (`_SC_NPROCESSORS_ONLN` is 84, as on Linux). |
| `flock` | It always succeeds: no other program uses the lock. |
| `sigaction` | It uses `signal`. Only `sa_handler` is used. |
| `malloc_usable_size` | The size of the block. |
| `dlopen`, `dlsym`, `dlclose`, `dlerror`, `dladdr` | `dlopen(NULL)` gives a handle for the program. `dlsym` finds no symbol, and all other `dlopen` calls fail. |

## Functions that fail

| Functions | Error |
| --- | --- |
| `pipe` | `ENOSYS` |
| `waitpid` | `ECHILD` |
| `posix_spawn`, `posix_spawnp` | `ENOSYS` (the file action functions succeed and keep nothing) |
| `tcgetattr`, `tcsetattr`, `tcdrain`, `tcflow`, `tcflush` | `ENOTTY` |

`cfmakeraw` and the `cfget*speed` and `cfset*speed` functions change
only the `struct termios`.

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

The C library adds `dlfcn.h` and `sys/utsname.h`. picolibc does not
declare `shm_open`, `shm_unlink`, `cfmakeraw`, `FIONREAD` or
`_SC_NPROCESSORS_ONLN`. Declare them in the program:

```c
int shm_open(const char *name, int flags, mode_t mode);
int shm_unlink(const char *name);
void cfmakeraw(struct termios *t);
#define FIONREAD 0x541B
#define _SC_NPROCESSORS_ONLN 84
```

## Tests

`tests/libc.c` checks these functions. `tests/run.sh` runs it in
`tools/sim68k.py` and `tools/simppc.py`.
