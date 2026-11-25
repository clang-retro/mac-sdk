/* Checks the POSIX threads of the C library (runtime/libc/thread.c) over
 * the Thread Manager: create and join, results, detached threads,
 * mutexes (normal, recursive, trylock), a condition variable between a
 * producer and a consumer, keys with destructors, pthread_once,
 * pthread_exit, sched_yield and usleep. tests/run.sh runs it in
 * tools/sim68k.py --os and tools/simppc.py, which fake the Thread
 * Manager, and checks the console output: one "ok" line per check, then
 * "all ok".
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

/* Prints one check's result, and counts the failures. */
static void check(const char *what, int ok) {
  printf("%s %s\n", ok ? "ok" : "FAILED", what);
  if (!ok)
    failures++;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int counter;
static char order[32];
static int order_len;

/* Adds 1 to the counter 100 times under the mutex. Every 25 steps, it
   records its letter in "order" and lets the other threads run. At step
   50, it also yields while it has the mutex. */
static void *count(void *arg) {
  int i;
  for (i = 0; i < 100; i++) {
    int v;
    pthread_mutex_lock(&lock);
    v = counter;
    if (i == 50)
      sched_yield();   /* the others wait for the mutex */
    counter = v + 1;
    if (i % 25 == 0)
      order[order_len++] = *(const char *)arg;
    pthread_mutex_unlock(&lock);
    if (i % 25 == 0)
      sched_yield();
  }
  return (void *)arg;
}

static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int queue[4];
static int queued, taken_sum;

/* Puts 1 to 20 in a queue of 4, and waits while the queue is full. */
static void *produce(void *arg) {
  int i;
  (void)arg;
  for (i = 1; i <= 20; i++) {
    pthread_mutex_lock(&lock);
    while (queued == 4)
      pthread_cond_wait(&ready, &lock);
    queue[queued++] = i;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
  }
  return NULL;
}

/* Takes 20 numbers from the queue and adds them, and waits while the
   queue is empty. */
static void *consume(void *arg) {
  int n;
  (void)arg;
  for (n = 0; n < 20; n++) {
    pthread_mutex_lock(&lock);
    while (queued == 0)
      pthread_cond_wait(&ready, &lock);
    taken_sum += queue[0];
    memmove(queue, queue + 1, sizeof queue[0] * (size_t)--queued);
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
  }
  return NULL;
}

static pthread_key_t key;
static int destroyed;

/* The key's destructor: counts the values it gets. */
static void destroy(void *v) {
  destroyed += *(int *)v;
}

/* Sets its own value for the key, yields, and checks that the value
   stayed its own. Ends with pthread_exit. */
static void *keyed(void *arg) {
  pthread_setspecific(key, arg);
  sched_yield();
  if (pthread_getspecific(key) != arg)
    pthread_exit((void *)1);
  pthread_exit((void *)2);
  return NULL;
}

static int once_calls;

/* The pthread_once function: counts its calls. */
static void once_init(void) {
  once_calls++;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int detached_ran;

/* A detached thread: calls pthread_once and says that it ran. */
static void *detached(void *arg) {
  (void)arg;
  pthread_once(&once, once_init);
  detached_ran = 1;
  return NULL;
}

int main(void) {
  pthread_t t[3];
  void *res[3];
  static const char names[] = "abc";
  int i, ok, one = 1, two = 2;
  pthread_mutex_t rec;
  pthread_mutexattr_t ma;
  pthread_attr_t attr;

  ok = 1;
  for (i = 0; i < 3; i++)
    ok &= pthread_create(&t[i], NULL, count, (void *)&names[i]) == 0;
  check("create", ok);
  for (i = 0; i < 3; i++)
    ok &= pthread_join(t[i], &res[i]) == 0 && res[i] == &names[i];
  check("join", ok);
  check("mutex", counter == 300);
  check("interleave", order_len == 12 && order[0] != order[1] && order[1] != order[2] &&
                          order[0] != order[2]);
  check("self", pthread_equal(pthread_self(), pthread_self()) &&
                    pthread_join(pthread_self(), NULL) == EDEADLK);

  pthread_mutexattr_init(&ma);
  pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&rec, &ma);
  check("recursive", pthread_mutex_lock(&rec) == 0 && pthread_mutex_lock(&rec) == 0 &&
                         pthread_mutex_unlock(&rec) == 0 && pthread_mutex_unlock(&rec) == 0 &&
                         pthread_mutex_unlock(&rec) == EPERM);
  check("trylock", pthread_mutex_trylock(&lock) == 0 && pthread_mutex_trylock(&lock) == EBUSY &&
                       pthread_mutex_unlock(&lock) == 0);

  pthread_create(&t[0], NULL, produce, NULL);
  pthread_create(&t[1], NULL, consume, NULL);
  pthread_join(t[0], NULL);
  pthread_join(t[1], NULL);
  check("condition", taken_sum == 210 && queued == 0);

  check("key", pthread_key_create(&key, destroy) == 0);
  pthread_setspecific(key, &one);
  pthread_create(&t[0], NULL, keyed, &one);
  pthread_create(&t[1], NULL, keyed, &two);
  pthread_join(t[0], &res[0]);
  pthread_join(t[1], &res[1]);
  check("exit and values", res[0] == (void *)2 && res[1] == (void *)2 &&
                               pthread_getspecific(key) == &one);
  check("destructor", destroyed == 3);

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&t[0], &attr, detached, NULL);
  pthread_create(&t[1], &attr, detached, NULL);
  check("usleep", usleep(50000) == 0);   /* lets them run */
  check("detached", detached_ran && pthread_join(t[0], NULL) == ESRCH);
  check("once", once_calls == 1);

  printf(failures ? "FAILED\n" : "all ok\n");
  return 0;
}
