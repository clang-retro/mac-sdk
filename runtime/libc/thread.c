/* POSIX threads (pthread.h) over the Thread Manager, and the functions
 * that wait: sched_yield, nanosleep and usleep.
 *
 * The threads are cooperative. A function that waits gives the processor
 * to the next ready thread with YieldToAnyThread, until the condition it
 * waits for is true. A thread that does not wait keeps the processor.
 * errno is one variable for all threads, so the functions that wait keep
 * its value across the switch.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <Gestalt.h>
#include <Threads.h>
#include <Events.h>
#include "macos.h"

/* The Thread Manager calls are newer than the oldest Systems; this file
   checks with Gestalt before it uses them. */
#pragma clang diagnostic ignored "-Wuser-defined-warnings"

/* The stack of a new thread when the attributes give no size. The Thread
   Manager's default is too small for printf. */
#define DEFAULT_STACK (32 * 1024L)

/* One thread. The main thread has a static record; each other thread has
   a record from malloc, in the list "threads". */
struct __mac_thread {
  struct __mac_thread *next;
  ThreadID id;
  void *(*start)(void *);
  void *arg;
  void *result;
  int done;
  int detached;
  jmp_buf exit;
  const void *values[PTHREAD_KEYS_MAX];
};

static struct __mac_thread main_thread;
static struct __mac_thread *threads;
static int manager;                    /* 0: not known, 1: present, -1: absent */
static unsigned char key_used[PTHREAD_KEYS_MAX];
static void (*key_destructor[PTHREAD_KEYS_MAX])(void *);

/* The Gestalt bit that says the Thread Manager can be used: on PowerPC,
   ThreadsLib, which the application imports weakly. */
#if TARGET_CPU_PPC
#define MANAGER_BIT gestaltThreadsLibraryPresent
#else
#define MANAGER_BIT gestaltThreadMgrPresent
#endif

/* Tells if the Thread Manager is present. */
static int have_manager(void) {
  long attr;
  if (manager == 0)
    manager = Gestalt(gestaltThreadMgrAttr, &attr) == noErr && (attr & (1L << MANAGER_BIT))
                  ? 1
                  : -1;
  return manager > 0;
}

/* Gives the processor to the next ready thread, and keeps errno. */
static void yield(void) {
  int saved = errno;
  if (have_manager())
    YieldToAnyThread();
  errno = saved;
}

/* Tells if the time abstime (CLOCK_REALTIME) has passed. */
static int passed(const struct timespec *abstime) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return now.tv_sec > abstime->tv_sec ||
         (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec);
}

/* Calls the key destructors for the values of thread t. */
static void run_destructors(struct __mac_thread *t) {
  int round, k, again = 1;
  for (round = 0; again && round < PTHREAD_DESTRUCTOR_ITERATIONS; round++) {
    again = 0;
    for (k = 0; k < PTHREAD_KEYS_MAX; k++) {
      void *v = (void *)t->values[k];
      if (key_used[k] && key_destructor[k] && v) {
        t->values[k] = NULL;
        key_destructor[k](v);
        again = 1;
      }
    }
  }
}

/* Removes thread t from the list and releases its record. */
static void release(struct __mac_thread *t) {
  struct __mac_thread **p;
  for (p = &threads; *p; p = &(*p)->next) {
    if (*p == t) {
      *p = t->next;
      break;
    }
  }
  free(t);
}

/* Tells if t is a thread that pthread_create made and that is not
   released. */
static int known(struct __mac_thread *t) {
  struct __mac_thread *p;
  for (p = threads; p; p = p->next)
    if (p == t)
      return 1;
  return 0;
}

/* The Thread Manager starts each new thread here. When this function
   returns, the Thread Manager disposes of the thread. */
static pascal void *thread_entry(void *param) {
  struct __mac_thread *t = param;
  if (setjmp(t->exit) == 0)
    t->result = t->start(t->arg);
  run_destructors(t);
  t->done = 1;
  if (t->detached)
    release(t);
  return NULL;
}

/* Sets the attributes to their defaults: joinable, default stack. */
int pthread_attr_init(pthread_attr_t *attr) {
  attr->__detach = PTHREAD_CREATE_JOINABLE;
  attr->__stacksize = 0;
  return 0;
}

/* Ends the use of the attributes. */
int pthread_attr_destroy(pthread_attr_t *attr) {
  (void)attr;
  return 0;
}

/* Sets if a new thread is joinable or detached. */
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state) {
  if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)
    return EINVAL;
  attr->__detach = state;
  return 0;
}

/* Gets if a new thread is joinable or detached. */
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state) {
  *state = attr->__detach;
  return 0;
}

/* Sets the stack size of a new thread, in bytes. */
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size) {
  if (size < 4096)
    return EINVAL;
  attr->__stacksize = size;
  return 0;
}

/* Gets the stack size of a new thread, in bytes. */
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size) {
  *size = attr->__stacksize ? attr->__stacksize : (size_t)DEFAULT_STACK;
  return 0;
}

/* Makes a cooperative thread that calls start(arg). The thread starts
   when the current thread waits. */
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
  struct __mac_thread *t;
  ThreadID id;
  Size stack = attr && attr->__stacksize ? (Size)attr->__stacksize : DEFAULT_STACK;
  if (!have_manager())
    return EAGAIN;
  t = calloc(1, sizeof *t);
  if (!t)
    return EAGAIN;
  t->start = start;
  t->arg = arg;
  t->detached = attr && attr->__detach == PTHREAD_CREATE_DETACHED;
  if (NewThread(kCooperativeThread, thread_entry, t, stack, kCreateIfNeeded,
                NULL, &id) != noErr) {
    free(t);
    return EAGAIN;
  }
  t->id = id;
  t->next = threads;
  threads = t;
  *thread = t;
  return 0;
}

/* Waits until a thread ends, and gives its result. */
int pthread_join(pthread_t thread, void **result) {
  if (thread == pthread_self())
    return EDEADLK;
  if (!known(thread))
    return ESRCH;
  if (thread->detached)
    return EINVAL;
  while (!thread->done)
    yield();
  if (result)
    *result = thread->result;
  release(thread);
  return 0;
}

/* Lets a thread release its record itself when it ends. */
int pthread_detach(pthread_t thread) {
  if (!known(thread))
    return ESRCH;
  if (thread->detached)
    return EINVAL;
  if (thread->done)
    release(thread);
  else
    thread->detached = 1;
  return 0;
}

/* Ends the current thread with a result. In the main thread, it waits
   until all other threads end, then ends the program. */
void pthread_exit(void *result) {
  pthread_t self = pthread_self();
  if (self == &main_thread) {
    run_destructors(self);
    for (;;) {
      struct __mac_thread *t;
      int running = 0;
      for (t = threads; t; t = t->next)
        running |= !t->done;
      if (!running)
        break;
      yield();
    }
    exit(0);
  }
  self->result = result;
  longjmp(self->exit, 1);
}

/* Cancels a thread. A cooperative thread cannot be stopped from outside,
   thus cancellation is not available. */
int pthread_cancel(pthread_t thread) {
  (void)thread;
  return ENOTSUP;
}

/* Gives the current thread. */
pthread_t pthread_self(void) {
  ThreadID id;
  struct __mac_thread *t;
  if (!threads || !have_manager() || GetCurrentThread(&id) != noErr)
    return &main_thread;
  for (t = threads; t; t = t->next)
    if (t->id == id && !t->done)
      return t;
  return &main_thread;
}

/* Tells if two thread values are the same thread. */
int pthread_equal(pthread_t a, pthread_t b) {
  return a == b;
}

/* Sets the mutex attributes to their defaults: a normal mutex. */
int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
  attr->__type = PTHREAD_MUTEX_DEFAULT;
  return 0;
}

/* Ends the use of the mutex attributes. */
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
  (void)attr;
  return 0;
}

/* Sets the mutex type: normal, recursive or error-checking. */
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
  if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK)
    return EINVAL;
  attr->__type = type;
  return 0;
}

/* Gets the mutex type. */
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
  *type = attr->__type;
  return 0;
}

/* Makes an unlocked mutex. */
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
  mutex->__type = attr ? attr->__type : PTHREAD_MUTEX_DEFAULT;
  mutex->__count = 0;
  mutex->__owner = NULL;
  return 0;
}

/* Ends the use of a mutex. A locked mutex gives EBUSY. */
int pthread_mutex_destroy(pthread_mutex_t *mutex) {
  return mutex->__owner ? EBUSY : 0;
}

/* Locks a mutex, and waits until the deadline (NULL: no deadline). */
static int lock(pthread_mutex_t *mutex, const struct timespec *abstime) {
  pthread_t self = pthread_self();
  if (mutex->__owner == self) {
    if (mutex->__type != PTHREAD_MUTEX_RECURSIVE)
      return EDEADLK;
    mutex->__count++;
    return 0;
  }
  while (mutex->__owner) {
    if (abstime && passed(abstime))
      return ETIMEDOUT;
    yield();
  }
  mutex->__owner = self;
  mutex->__count = 1;
  return 0;
}

/* Locks a mutex, and waits while another thread has it. A thread that
   locks a non-recursive mutex again gets EDEADLK. */
int pthread_mutex_lock(pthread_mutex_t *mutex) {
  return lock(mutex, NULL);
}

/* Locks a mutex, or gives EBUSY when it is locked. */
int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  pthread_t self = pthread_self();
  if (mutex->__owner && (mutex->__owner != self ||
                         mutex->__type != PTHREAD_MUTEX_RECURSIVE))
    return EBUSY;
  return lock(mutex, NULL);
}

/* Locks a mutex, and gives ETIMEDOUT at the time abstime. */
int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime) {
  return lock(mutex, abstime);
}

/* Unlocks a mutex that the current thread has. */
int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  if (mutex->__owner != pthread_self())
    return EPERM;
  if (--mutex->__count == 0)
    mutex->__owner = NULL;
  return 0;
}

/* Sets the condition variable attributes to their defaults. */
int pthread_condattr_init(pthread_condattr_t *attr) {
  attr->__reserved = 0;
  return 0;
}

/* Ends the use of the condition variable attributes. */
int pthread_condattr_destroy(pthread_condattr_t *attr) {
  (void)attr;
  return 0;
}

/* Makes a condition variable. */
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
  (void)attr;
  cond->__seq = 0;
  return 0;
}

/* Ends the use of a condition variable. */
int pthread_cond_destroy(pthread_cond_t *cond) {
  (void)cond;
  return 0;
}

/* Unlocks the mutex, waits for a signal or the deadline (NULL: none),
   then locks the mutex again. */
static int wait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                const struct timespec *abstime) {
  unsigned long seq = cond->__seq;
  unsigned count = mutex->__count;
  pthread_t self = pthread_self();
  int err = 0;
  if (mutex->__owner != self)
    return EPERM;
  mutex->__owner = NULL;
  mutex->__count = 0;
  while (cond->__seq == seq) {
    if (abstime && passed(abstime)) {
      err = ETIMEDOUT;
      break;
    }
    yield();
  }
  while (mutex->__owner)
    yield();
  mutex->__owner = self;
  mutex->__count = count;
  return err;
}

/* Waits for a signal on the condition variable. */
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  return wait(cond, mutex, NULL);
}

/* Waits for a signal, and gives ETIMEDOUT at the time abstime. */
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime) {
  return wait(cond, mutex, abstime);
}

/* Wakes the threads that wait on the condition variable. POSIX permits
   this to wake more than one. */
int pthread_cond_signal(pthread_cond_t *cond) {
  cond->__seq++;
  return 0;
}

/* Wakes all threads that wait on the condition variable. */
int pthread_cond_broadcast(pthread_cond_t *cond) {
  cond->__seq++;
  return 0;
}

/* Makes a key for a value per thread, with a destructor (or NULL). */
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
  unsigned k;
  struct __mac_thread *t;
  for (k = 0; k < PTHREAD_KEYS_MAX; k++) {
    if (!key_used[k]) {
      key_used[k] = 1;
      key_destructor[k] = destructor;
      main_thread.values[k] = NULL;
      for (t = threads; t; t = t->next)
        t->values[k] = NULL;
      *key = k;
      return 0;
    }
  }
  return EAGAIN;
}

/* Ends the use of a key. The destructor is not called. */
int pthread_key_delete(pthread_key_t key) {
  if (key >= PTHREAD_KEYS_MAX || !key_used[key])
    return EINVAL;
  key_used[key] = 0;
  return 0;
}

/* Gives the current thread's value for a key. */
void *pthread_getspecific(pthread_key_t key) {
  if (key >= PTHREAD_KEYS_MAX)
    return NULL;
  return (void *)pthread_self()->values[key];
}

/* Sets the current thread's value for a key. */
int pthread_setspecific(pthread_key_t key, const void *value) {
  if (key >= PTHREAD_KEYS_MAX || !key_used[key])
    return EINVAL;
  pthread_self()->values[key] = value;
  return 0;
}

/* Calls init one time. A second thread waits until the first call
   ends. */
int pthread_once(pthread_once_t *once, void (*init)(void)) {
  if (once->__done == 0) {
    once->__done = 1;
    init();
    once->__done = 2;
  }
  while (once->__done != 2)
    yield();
  return 0;
}

/* Gives the processor to the next ready thread. */
int sched_yield(void) {
  yield();
  return 0;
}

/* Waits for a time, and lets the other threads run. The clock is the
   Mac's tick count, 60 per second. */
int nanosleep(const struct timespec *req, struct timespec *rem) {
  unsigned long ticks, end;
  if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
    errno = EINVAL;
    return -1;
  }
  ticks = (unsigned long)req->tv_sec * 60 +
          (unsigned long)((req->tv_nsec + 16666666L) / 16666667L);
  end = TickCount() + ticks;
  while ((long)(end - TickCount()) > 0)
    yield();
  if (rem) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}

/* Waits for a number of microseconds. */
int usleep(useconds_t usec) {
  struct timespec t;
  t.tv_sec = (time_t)(usec / 1000000);
  t.tv_nsec = (long)(usec % 1000000) * 1000;
  return nanosleep(&t, NULL);
}

/* Lets the other threads run, and about once a tick the other programs
   too: a cooperative Mac switches programs only in the Event Manager's
   calls. EventAvail with an empty mask takes no event from the program.
   It is called only when the program has started QuickDraw (CurrentA5
   points to a port), because the Event Manager needs it. */
void __mac_wait(void) {
  static unsigned long last;
  unsigned long now;
  yield();
  now = TickCount();
  if (now != last) {
    long *a5 = *(long **)0x904;
    last = now;
    if (a5 && *a5) {
      EventRecord ev;
      int saved = errno;
      EventAvail(0, &ev);
      errno = saved;
    }
  }
}
