/* thread_local (and _Thread_local) variables for the classic Mac OS.
 *
 * The Thread Manager's threads have no thread pointer, so clang uses
 * emulated TLS for these targets: each access to a thread_local variable
 * calls __emutls_get_address with the variable's control record, which
 * gives the address of the current thread's copy. A copy is made at the
 * first access in each thread, from the variable's initial value. The
 * copies of a thread are released when it ends (thread.c runs the key
 * destructors).
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The control record that clang makes for each thread_local variable
   (__emutls_v.NAME). */
struct __emutls_control {
  size_t size;   /* bytes */
  size_t align;  /* bytes, a power of 2 */
  union {
    uintptr_t index; /* 0: no index yet; otherwise 1 + the variable's slot */
    void *address;
  } object;
  void *value;   /* the initial value (__emutls_t.NAME), or NULL for zero */
};

/* The copies of one thread: slots[i] is the copy of the variable with
   index i + 1. */
struct copies {
  uintptr_t count;
  void *slots[];
};

static pthread_key_t key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static uintptr_t next_index;

/* Releases the copies of a thread that ends. */
static void release_copies(void *p) {
  struct copies *c = p;
  uintptr_t i;
  for (i = 0; i < c->count; i++)
    free(c->slots[i]);
  free(c);
}

/* Makes the key that holds each thread's copies. */
static void make_key(void) {
  pthread_key_create(&key, release_copies);
}

/* Gives the address of the current thread's copy of a thread_local
   variable, and makes the copy at the first access. Gives NULL when
   there is no memory. */
void *__emutls_get_address(struct __emutls_control *control) {
  struct copies *c;
  uintptr_t i;
  void *obj;
  pthread_once(&key_once, make_key);
  if (control->object.index == 0)
    control->object.index = ++next_index;
  i = control->object.index - 1;
  c = pthread_getspecific(key);
  if (c == NULL || i >= c->count) {
    uintptr_t n = next_index + 8;
    struct copies *bigger = realloc(c, sizeof *c + n * sizeof c->slots[0]);
    if (bigger == NULL)
      return NULL;
    memset(bigger->slots + (c ? bigger->count : 0), 0,
           (n - (c ? bigger->count : 0)) * sizeof bigger->slots[0]);
    bigger->count = n;
    c = bigger;
    pthread_setspecific(key, c);
  }
  obj = c->slots[i];
  if (obj == NULL) {
    size_t align = control->align < sizeof(void *) ? sizeof(void *) : control->align;
    size_t size = (control->size + align - 1) & ~(align - 1);
    obj = aligned_alloc(align, size ? size : align);
    if (obj == NULL)
      return NULL;
    if (control->value)
      memcpy(obj, control->value, control->size);
    else
      memset(obj, 0, control->size);
    c->slots[i] = obj;
  }
  return obj;
}
