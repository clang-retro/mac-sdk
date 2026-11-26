/* softfloat68k.c - checks the soft float of runtime/softfloat-m68k.S,
 * which libmacrt.a has for 68k, against vectors from the reference
 * runtime/softfloat.c (softfloat-vectors.h, which tests/softfloat.c
 * --vectors writes). tests/run.sh runs it in tools/sim68k.py. It draws
 * "all ok", or the first failures.
 *
 * Part of mac-sdk.
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "softfloat-vectors.h"

/* The functions under test, as clang calls them for float arithmetic. */
float __addsf3(float, float);
float __subsf3(float, float);
float __mulsf3(float, float);
float __divsf3(float, float);
int __lesf2(float, float);
int __gesf2(float, float);
int __unordsf2(float, float);
int32_t __fixsfsi(float);
float __floatsisf(int32_t);

/* Gives the float with the bits u. */
static float float_of(uint32_t u) {
  float f;
  memcpy(&f, &u, 4);
  return f;
}

/* Gives the bits of the float f. */
static uint32_t bits_of(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}

/* Gives 1 when the bits u are a NaN. */
static int is_nan(uint32_t u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }

/* Gives the sign of a comparison result: -1, 0 or 1. */
static int sgn(int v) { return v < 0 ? -1 : v > 0; }

/* Runs one vector and gives its result. */
static uint32_t run(const struct sf_vector *v) {
  float a = float_of(v->a), b = float_of(v->b);
  switch (v->op) {
    case 0: return bits_of(__addsf3(a, b));
    case 1: return bits_of(__subsf3(a, b));
    case 2: return bits_of(__mulsf3(a, b));
    case 3: return bits_of(__divsf3(a, b));
    case 4: return (uint32_t)sgn(__lesf2(a, b));
    case 5: return (uint32_t)sgn(__gesf2(a, b));
    case 6: return (uint32_t)(__unordsf2(a, b) != 0);
    case 7: return (uint32_t)__fixsfsi(a);
    default: return bits_of(__floatsisf((int32_t)v->a));
  }
}

/* Runs all the vectors. */
int main(void) {
  static const char *names[] = {"add", "sub", "mul", "div", "le", "ge", "unord", "fix", "float"};
  size_t i, n = sizeof sf_vectors / sizeof *sf_vectors;
  int failures = 0;
  for (i = 0; i < n; i++) {
    const struct sf_vector *v = &sf_vectors[i];
    uint32_t r = run(v);
    if (r == v->r || (v->op < 4 && is_nan(r) && is_nan(v->r))) continue;
    if (failures++ < 10)
      printf("FAIL %s(%08lx, %08lx): %08lx, not %08lx\n", names[v->op], (unsigned long)v->a,
             (unsigned long)v->b, (unsigned long)r, (unsigned long)v->r);
  }
  printf("softfloat68k: %lu vectors, %d failures\n", (unsigned long)n, failures);
  printf(failures ? "FAIL\n" : "all ok\n");
  return 0;
}
