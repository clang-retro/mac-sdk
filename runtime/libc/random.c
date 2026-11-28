/* getentropy for the classic Mac OS, which has no random device. picolibc's
 * arc4random and libstdc++'s std::random_device read it.
 *
 * The function keeps a 32-byte pool. Each call mixes new samples into
 * the pool with SHA-256 (FIPS 180-4), then makes the output with
 * SHA-256 from the pool, then replaces the pool, so that the output does
 * not show the pool or the earlier output. The samples are:
 *
 *  - the jitter of the fine clock (UpTime or Microseconds, see time.c)
 *    against a counted loop: interrupts (the VBL task, the sound, ADB,
 *    the disk, the network) change the count;
 *  - the fine clock, TickCount and the Mac's date and time;
 *  - the mouse position and button and the keyboard map, from low memory;
 *  - the stack pointer and memory addresses that change from run to run.
 *
 * The first call takes more jitter samples than the calls after it. On a
 * Mac without a fine clock (68k System 6), each jitter sample waits for a
 * tick, so the first call takes about 0.2 second.
 *
 * A program can give its own getentropy: the linker then does not take
 * this file.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <Events.h>
#include <LowMem.h>
#include <OSUtils.h>
#include "macos.h"

/* getentropy gives at most this many bytes in one call (as on OpenBSD
   and Linux). */
#define MAX_REQUEST 256

/* Jitter samples for the first call, and for each call after it. */
#define FIRST_SAMPLES 64
#define NEXT_SAMPLES 8
/* The same, when the clock steps in ticks (each sample waits a tick). */
#define FIRST_SAMPLES_TICKS 12
#define NEXT_SAMPLES_TICKS 2

/* The state of a SHA-256 calculation. */
struct sha256 {
  uint32_t h[8];        /* the hash value */
  uint8_t block[64];    /* the bytes of the block that is not full */
  uint32_t used;        /* the bytes in block */
  uint64_t length;      /* all the bytes, for the padding */
};

/* The round constants of SHA-256. */
static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/* Rotates x right by n bits (0 < n < 32). */
static uint32_t ror(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32 - n));
}

/* Sets c to the start of a new SHA-256 calculation. */
static void sha256_init(struct sha256 *c) {
  static const uint32_t iv[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  };
  memcpy(c->h, iv, sizeof iv);
  c->used = 0;
  c->length = 0;
}

/* Mixes one full 64-byte block into the hash value of c. */
static void sha256_block(struct sha256 *c, const uint8_t *p) {
  uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
  unsigned i;
  for (i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
           (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (; i < 64; i++)
    w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
           w[i - 7] + (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
  a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
  e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
  for (i = 0; i < 64; i++) {
    t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
  explicit_bzero(w, sizeof w);
}

/* Adds n bytes at p to the calculation c. */
static void sha256_add(struct sha256 *c, const void *p, size_t n) {
  const uint8_t *s = p;
  c->length += n;
  while (n) {
    size_t k = 64 - c->used;
    if (k > n)
      k = n;
    memcpy(c->block + c->used, s, k);
    c->used += (uint32_t)k;
    s += k;
    n -= k;
    if (c->used == 64) {
      sha256_block(c, c->block);
      c->used = 0;
    }
  }
}

/* Ends the calculation c, puts the 32-byte hash in out, and clears c. */
static void sha256_end(struct sha256 *c, uint8_t out[32]) {
  uint64_t bits = c->length * 8;
  uint8_t pad[8];
  unsigned i;
  static const uint8_t one = 0x80, zero = 0;
  sha256_add(c, &one, 1);
  while (c->used != 56)
    sha256_add(c, &zero, 1);
  for (i = 0; i < 8; i++)
    pad[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha256_add(c, pad, 8);
  for (i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(c->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)c->h[i];
  }
  explicit_bzero(c, sizeof *c);
}

/* The pool, the count of calls, and 1 after the first call. */
static uint8_t pool[32];
static uint32_t calls;
static int seeded;

/* Adds n jitter samples to c. Each sample counts the turns of a loop
   until the fine clock changes, and adds the count and the clock. */
static void add_jitter(struct sha256 *c, unsigned n) {
  struct { unsigned long long now; uint32_t turns; } s;
  unsigned long long start;
  while (n--) {
    start = __mac_microseconds();
    s.turns = 0;
    do {
      s.turns++;
      s.now = __mac_microseconds();
    } while (s.now == start);
    sha256_add(c, &s, sizeof s);
  }
}

/* Adds the state of the Mac that changes from call to call to c: the
   clocks, the mouse, the keyboard and the stack address. */
static void add_state(struct sha256 *c) {
  struct {
    unsigned long long us;
    unsigned long ticks, date;
    Point mouse;
    uint8_t button;
    uint8_t keys[16];
    void *stack;
  } s;
  memset(&s, 0, sizeof s);
  s.us = __mac_microseconds();
  s.ticks = TickCount();
  GetDateTime(&s.date);
  s.mouse = LMGetRawMouseLocation();
  s.button = LMGetMouseButtonState();
  memcpy(s.keys, (const void *)0x174, sizeof s.keys);   /* KeyMap */
  s.stack = &s;
  sha256_add(c, &s, sizeof s);
}

/* Adds the values that change from run to run, but not from call to
   call, to c: the memory addresses of this program and its heap. */
static void add_run(struct sha256 *c) {
  struct {
    const void *global, *code;
    uint32_t zone, heap_end, stack_base;
  } s;
  s.global = pool;
  s.code = (const void *)(uintptr_t)&add_run;
  s.zone = *(const uint32_t *)0x2AA;        /* ApplZone */
  s.heap_end = *(const uint32_t *)0x114;    /* HeapEnd */
  s.stack_base = *(const uint32_t *)0x908;  /* CurStackBase */
  sha256_add(c, &s, sizeof s);
}

/* Mixes new samples into the pool. */
static void stir(void) {
  struct sha256 c;
  int fine = __mac_microseconds_fine();
  sha256_init(&c);
  sha256_add(&c, "clang-retro mac getentropy pool", 31);
  sha256_add(&c, pool, sizeof pool);
  sha256_add(&c, &calls, sizeof calls);
  add_state(&c);
  if (!seeded) {
    add_run(&c);
    add_jitter(&c, fine ? FIRST_SAMPLES : FIRST_SAMPLES_TICKS);
    seeded = 1;
  } else {
    add_jitter(&c, fine ? NEXT_SAMPLES : NEXT_SAMPLES_TICKS);
  }
  add_state(&c);
  sha256_end(&c, pool);
}

/* Fills buf with len random bytes (at most 256). Gives 0, or -1 with
   errno set: EIO when len is more than 256, EFAULT when buf is NULL. */
int getentropy(void *buf, size_t len) {
  struct sha256 c;
  uint8_t block[32];
  uint8_t *out = buf;
  uint32_t i;
  size_t k;
  if (len > MAX_REQUEST) {
    errno = EIO;
    return -1;
  }
  if (!buf && len) {
    errno = EFAULT;
    return -1;
  }
  calls++;
  stir();
  for (i = 0; len; i++) {
    sha256_init(&c);
    sha256_add(&c, "out", 3);
    sha256_add(&c, pool, sizeof pool);
    sha256_add(&c, &i, sizeof i);
    sha256_end(&c, block);
    k = len < sizeof block ? len : sizeof block;
    memcpy(out, block, k);
    out += k;
    len -= k;
  }
  /* A new pool, so that the pool does not show this output. */
  sha256_init(&c);
  sha256_add(&c, "next", 4);
  sha256_add(&c, pool, sizeof pool);
  sha256_end(&c, pool);
  explicit_bzero(block, sizeof block);
  return 0;
}
