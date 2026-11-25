/* The atomic operations that clang calls when the CPU cannot do them
 * itself: the __sync_* functions (on the 68000, which has no CAS) and
 * the __atomic_* functions (8-byte and odd-size objects, and the generic
 * ones for any size).
 *
 * The classic Mac OS threads are cooperative (runtime/libc/thread.c): a
 * thread switch occurs only in a call that waits. Thus a plain read,
 * change and write is atomic between threads. These functions are not
 * atomic with respect to interrupt code, such as a completion routine;
 * do not use the same object from interrupt code and from a thread.
 *
 * clang knows these names as builtins, so each function has a C name
 * and an assembler label that gives it the real name.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <stddef.h>

/* From string.c; the runtime has no C library headers. */
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

typedef unsigned char u1;
typedef unsigned short u2;
typedef unsigned int u4;
typedef unsigned long long u8;

/* The __sync_ functions for one size N with type T:
   fetch_and_OP and OP_and_fetch for add, sub, or, and, xor and nand,
   the two compare-and-swap functions, lock_test_and_set and
   lock_release. */
#define SYNC_OP(N, T, OP, EXPR)                                                \
  T sync_fetch_and_##OP##_##N(volatile T *p, T v)                              \
      __asm__("__sync_fetch_and_" #OP "_" #N);                                 \
  /* Changes *p and gives the old value. */                                    \
  T sync_fetch_and_##OP##_##N(volatile T *p, T v) {                            \
    T old = *p;                                                                \
    *p = (T)(EXPR);                                                            \
    return old;                                                                \
  }                                                                            \
  T sync_##OP##_and_fetch_##N(volatile T *p, T v)                              \
      __asm__("__sync_" #OP "_and_fetch_" #N);                                 \
  /* Changes *p and gives the new value. */                                    \
  T sync_##OP##_and_fetch_##N(volatile T *p, T v) {                            \
    T old = *p;                                                                \
    return *p = (T)(EXPR);                                                     \
  }

#define SYNC(N, T)                                                             \
  SYNC_OP(N, T, add, old + v)                                                  \
  SYNC_OP(N, T, sub, old - v)                                                  \
  SYNC_OP(N, T, or, old | v)                                                   \
  SYNC_OP(N, T, and, old & v)                                                  \
  SYNC_OP(N, T, xor, old ^ v)                                                  \
  SYNC_OP(N, T, nand, ~(old & v))                                              \
  T sync_val_cas_##N(volatile T *p, T expected, T desired)                     \
      __asm__("__sync_val_compare_and_swap_" #N);                              \
  /* Sets *p to desired if it holds expected; gives the old value. */          \
  T sync_val_cas_##N(volatile T *p, T expected, T desired) {                   \
    T old = *p;                                                                \
    if (old == expected)                                                       \
      *p = desired;                                                            \
    return old;                                                                \
  }                                                                            \
  _Bool sync_bool_cas_##N(volatile T *p, T expected, T desired)                \
      __asm__("__sync_bool_compare_and_swap_" #N);                             \
  /* Sets *p to desired if it holds expected; tells if it did. */              \
  _Bool sync_bool_cas_##N(volatile T *p, T expected, T desired) {              \
    if (*p != expected)                                                        \
      return 0;                                                                \
    *p = desired;                                                              \
    return 1;                                                                  \
  }                                                                            \
  T sync_lock_test_and_set_##N(volatile T *p, T v)                             \
      __asm__("__sync_lock_test_and_set_" #N);                                 \
  /* Sets *p to v and gives the old value. */                                  \
  T sync_lock_test_and_set_##N(volatile T *p, T v) {                           \
    T old = *p;                                                                \
    *p = v;                                                                    \
    return old;                                                                \
  }                                                                            \
  void sync_lock_release_##N(volatile T *p) __asm__("__sync_lock_release_" #N); \
  /* Sets *p to 0. */                                                          \
  void sync_lock_release_##N(volatile T *p) { *p = 0; }

SYNC(1, u1)
SYNC(2, u2)
SYNC(4, u4)
SYNC(8, u8)

void sync_synchronize(void) __asm__("__sync_synchronize");
/* A full memory barrier. One processor runs all threads, so it only
   keeps the compiler from moving memory accesses across it. */
void sync_synchronize(void) { __asm__ __volatile__("" ::: "memory"); }

/* The sized __atomic_ functions for one size N with type T. The memory
   order arguments do not matter on one processor. */
#define ATOMIC_OP(N, T, OP, EXPR)                                              \
  T atomic_fetch_##OP##_##N(volatile T *p, T v, int order)                     \
      __asm__("__atomic_fetch_" #OP "_" #N);                                   \
  /* Changes *p and gives the old value. */                                    \
  T atomic_fetch_##OP##_##N(volatile T *p, T v, int order) {                   \
    T old = *p;                                                                \
    (void)order;                                                               \
    *p = (T)(EXPR);                                                            \
    return old;                                                                \
  }

#define ATOMIC(N, T)                                                           \
  ATOMIC_OP(N, T, add, old + v)                                                \
  ATOMIC_OP(N, T, sub, old - v)                                                \
  ATOMIC_OP(N, T, or, old | v)                                                 \
  ATOMIC_OP(N, T, and, old & v)                                                \
  ATOMIC_OP(N, T, xor, old ^ v)                                                \
  ATOMIC_OP(N, T, nand, ~(old & v))                                            \
  T atomic_load_##N(const volatile T *p, int order) __asm__("__atomic_load_" #N); \
  /* Gives *p. */                                                              \
  T atomic_load_##N(const volatile T *p, int order) {                          \
    (void)order;                                                               \
    return *p;                                                                 \
  }                                                                            \
  void atomic_store_##N(volatile T *p, T v, int order)                         \
      __asm__("__atomic_store_" #N);                                           \
  /* Sets *p to v. */                                                          \
  void atomic_store_##N(volatile T *p, T v, int order) {                       \
    (void)order;                                                               \
    *p = v;                                                                    \
  }                                                                            \
  T atomic_exchange_##N(volatile T *p, T v, int order)                         \
      __asm__("__atomic_exchange_" #N);                                        \
  /* Sets *p to v and gives the old value. */                                  \
  T atomic_exchange_##N(volatile T *p, T v, int order) {                       \
    T old = *p;                                                                \
    (void)order;                                                               \
    *p = v;                                                                    \
    return old;                                                                \
  }                                                                            \
  _Bool atomic_cas_##N(volatile T *p, T *expected, T desired, int s, int f)    \
      __asm__("__atomic_compare_exchange_" #N);                                \
  /* Sets *p to desired if it holds *expected and tells if it did;        \
     otherwise puts the value of *p in *expected. */                           \
  _Bool atomic_cas_##N(volatile T *p, T *expected, T desired, int s, int f) {  \
    (void)s;                                                                   \
    (void)f;                                                                   \
    if (*p == *expected) {                                                     \
      *p = desired;                                                            \
      return 1;                                                                \
    }                                                                          \
    *expected = *p;                                                            \
    return 0;                                                                  \
  }

ATOMIC(1, u1)
ATOMIC(2, u2)
ATOMIC(4, u4)
ATOMIC(8, u8)

/* The generic __atomic_ functions, for an object of any size. */
void atomic_load(size_t size, const volatile void *src, void *dst, int order)
    __asm__("__atomic_load");
/* Copies the object at src to dst. */
void atomic_load(size_t size, const volatile void *src, void *dst, int order) {
  (void)order;
  memcpy(dst, (const void *)src, size);
}

void atomic_store(size_t size, volatile void *dst, void *src, int order)
    __asm__("__atomic_store");
/* Copies src to the object at dst. */
void atomic_store(size_t size, volatile void *dst, void *src, int order) {
  (void)order;
  memcpy((void *)dst, src, size);
}

void atomic_exchange(size_t size, volatile void *p, void *v, void *old, int order)
    __asm__("__atomic_exchange");
/* Copies the object at p to old, then v to the object at p. */
void atomic_exchange(size_t size, volatile void *p, void *v, void *old, int order) {
  (void)order;
  memcpy(old, (const void *)p, size);
  memcpy((void *)p, v, size);
}

_Bool atomic_compare_exchange(size_t size, volatile void *p, void *expected,
                              void *desired, int s, int f)
    __asm__("__atomic_compare_exchange");
/* Copies desired to the object at p if it equals *expected, and tells if
   it did; otherwise copies the object at p to expected. */
_Bool atomic_compare_exchange(size_t size, volatile void *p, void *expected,
                              void *desired, int s, int f) {
  (void)s;
  (void)f;
  if (memcmp((const void *)p, expected, size) == 0) {
    memcpy((void *)p, desired, size);
    return 1;
  }
  memcpy(expected, (const void *)p, size);
  return 0;
}

_Bool atomic_is_lock_free(size_t size, const volatile void *p)
    __asm__("__atomic_is_lock_free");
/* Tells if objects of this size are lock free: all are, since no
   operation here takes a lock. */
_Bool atomic_is_lock_free(size_t size, const volatile void *p) {
  (void)size;
  (void)p;
  return 1;
}
