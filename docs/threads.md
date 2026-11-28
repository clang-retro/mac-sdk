# Threads

The C library has POSIX threads (`pthread.h`) for 68k and PowerPC. They
use the Thread Manager. `runtime/libc/thread.c` has the code.

## Requirements

- The Thread Manager. It is part of System 7.5 and later. For System 7.0
  to 7.1, install the Thread Manager extension.
- The library examines `Gestalt('thds')` before it uses the Thread
  Manager. Without the Thread Manager, `pthread_create` gives `EAGAIN`.
  The other functions operate as if only the main thread exists.
- On PowerPC, the application imports the Thread Manager calls from
  ThreadsLib. The import is weak, thus the application also starts on a
  System without ThreadsLib. The library uses the Gestalt bit
  `gestaltThreadsLibraryPresent` on PowerPC.

## How the threads run

The threads are cooperative (`kCooperativeThread`). One thread runs at
a time. The running thread continues until it calls a function that
waits. Then the Thread Manager runs the next ready thread.

These functions can let other threads run:

| Function | When it lets other threads run |
| --- | --- |
| `pthread_join` | Until the thread ends |
| `pthread_mutex_lock`, `pthread_mutex_timedlock` | While another thread has the mutex |
| `pthread_cond_wait`, `pthread_cond_timedwait` | Until a signal |
| `pthread_exit` in the main thread | Until all other threads end |
| `pthread_once` | While another thread runs the function |
| `sched_yield` | One time |
| `nanosleep`, `usleep`, `sleep` | Until the time passes |

A thread that does not call these functions stops all other threads.
In a long loop, call `sched_yield`. The main thread is the application.
It must also call one of these functions, for example in its event loop,
to let the other threads run.

A new thread does not start immediately. It starts when the thread that
made it waits.

## Functions

- Threads: `pthread_create`, `pthread_join`, `pthread_detach`,
  `pthread_exit`, `pthread_self`, `pthread_equal`.
- Thread attributes: `pthread_attr_init`, `pthread_attr_destroy`,
  `pthread_attr_setdetachstate`, `pthread_attr_getdetachstate`,
  `pthread_attr_setstacksize`, `pthread_attr_getstacksize`.
- Mutexes: `pthread_mutex_init`, `pthread_mutex_destroy`,
  `pthread_mutex_lock`, `pthread_mutex_trylock`,
  `pthread_mutex_timedlock`, `pthread_mutex_unlock`, and the attributes
  `pthread_mutexattr_init`, `pthread_mutexattr_destroy`,
  `pthread_mutexattr_settype`, `pthread_mutexattr_gettype`. The types
  are `PTHREAD_MUTEX_NORMAL`, `PTHREAD_MUTEX_RECURSIVE` and
  `PTHREAD_MUTEX_ERRORCHECK`.
- Condition variables: `pthread_cond_init`, `pthread_cond_destroy`,
  `pthread_cond_wait`, `pthread_cond_timedwait`, `pthread_cond_signal`,
  `pthread_cond_broadcast`, `pthread_condattr_init`,
  `pthread_condattr_destroy`.
- Values for each thread: `pthread_key_create`, `pthread_key_delete`,
  `pthread_getspecific`, `pthread_setspecific`. There are 64 keys
  (`PTHREAD_KEYS_MAX`). The destructors run when a thread ends.
- `pthread_once`.
- `sched_yield`, `nanosleep`, `usleep` and `clock_gettime`.

## Thread-local variables

C (`_Thread_local`, `thread_local`) and C++ (`thread_local`) programs
can declare thread-local variables. The CPUs of the classic Mac OS have
no thread pointer. Thus clang uses emulated TLS for these targets: each
access calls `__emutls_get_address` (`runtime/libc/emutls.c`).

- Each thread gets its own copy of a variable at its first access. The
  copy starts with the initial value of the variable.
- The copies of a thread are released when the thread ends.
- An access is a function call. Thus keep the address of a thread-local
  variable in a local variable when a loop uses it frequently.
- If there is no memory for a copy, `__emutls_get_address` gives
  `NULL`, and the program fails at the access.

clang selects emulated TLS for these targets since clang-retro/llvm
#57. An older clang needs the option `-femulated-tls`, and on PowerPC it
cannot compile thread-local variables.

## Limits

- A thread that locks a normal mutex again gets `EDEADLK`. It does not
  wait for ever.
- `pthread_cond_signal` wakes all threads that wait on the condition
  variable, the same as `pthread_cond_broadcast`. POSIX permits this.
  Always examine the condition again after the wait.
- The default stack of a new thread is 32 KB. Use
  `pthread_attr_setstacksize` for a larger or smaller stack.
- `errno` is one variable for all threads. The functions that wait keep
  its value across a switch to another thread.
- `CLOCK_REALTIME`, `CLOCK_MONOTONIC` and the sleep functions use the
  fine clock, which steps in microseconds (in ticks, 60 each second, on
  System 6). See [Clocks](posix.md#clocks).
- There are no preemptive threads, no thread priorities and no read-write
  locks. A thread cannot be cancelled: `pthread_cancel` gives `ENOTSUP`.
- The C++ library uses these threads (thread model `posix`). Thus
  every C++ program contains the thread functions; they call the
  Thread Manager only when the program makes a thread.
- The atomic operations that the CPU cannot do (all of them on the
  68000, 8-byte ones on PowerPC) are functions in the runtime library
  (`runtime/atomic.c`). They are atomic between threads, but not with
  respect to interrupt code such as completion routines.

## Tests

`tests/thread.c` checks the functions. `tests/run.sh` runs it in
`tools/sim68k.py --os` (as a 68000 and a 68020) and in
`tools/simppc.py`. The two simulators have a fake Thread Manager. It
keeps the registers of each thread that does not run, and runs the
ready threads in turn.
