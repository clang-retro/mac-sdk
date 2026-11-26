/* softfloat.c - single-precision soft float for the 68000.
 *
 * clang calls these functions for float arithmetic on a 68k without an
 * FPU. They replace the ones of compiler-rt in libmacrt.a for 68k:
 * __addsf3, __subsf3, __mulsf3, __divsf3, the comparisons (__eqsf2,
 * __nesf2, __ltsf2, __lesf2, __gtsf2, __gesf2, __unordsf2), __fixsfsi
 * and __floatsisf.
 *
 * The results are those of IEEE 754 binary32 with rounding to nearest,
 * ties to even, and with subnormal numbers, the same as compiler-rt's.
 * A NaN operand gives that NaN, quiet; an invalid operation gives the
 * default NaN 0x7FC00000.
 *
 * The code is written for the 68000: it multiplies only 16-bit values
 * (one mulu instruction each), shifts by constant amounts where it can,
 * and has no 64-bit arithmetic. compiler-rt's versions call __muldi3
 * and __mulsi3, which the 68000 does not have in hardware.
 *
 * tests/softfloat.c compares every function with the host's float
 * arithmetic.
 *
 * Part of mac-sdk.
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include <stdint.h>

#ifndef SF_NAME
#define SF_NAME(n) __##n
#endif

/* The parts of a float: sign, biased exponent, fraction. */
#define SIGN 0x80000000u
#define EXP_MASK 0x7F800000u
#define FRAC_MASK 0x007FFFFFu
#define IMPLICIT 0x00800000u
#define QUIET 0x00400000u
#define DEFAULT_NAN 0x7FC00000u
#define INF 0x7F800000u

/* A float as its bits, and its bits as a float. */
typedef union {
  float f;
  uint32_t u;
} sf_bits;

/* Gives the bits of the float a. */
static inline uint32_t bits_of(float a) {
  sf_bits b;
  b.f = a;
  return b.u;
}

/* Gives the float with the bits u. */
static inline float float_of(uint32_t u) {
  sf_bits b;
  b.u = u;
  return b.f;
}

/* Gives the number of zero bits above the highest one bit of m, which
 * must not be zero. The 68000 has no instruction for this. */
static inline int clz32(uint32_t m) {
  int k = 0;
  if (!(m & 0xFFFF0000u)) {
    m <<= 16;
    k = 16;
  }
  if (!(m & 0xFF000000u)) {
    m <<= 8;
    k += 8;
  }
  if (!(m & 0xF0000000u)) {
    m <<= 4;
    k += 4;
  }
  if (!(m & 0xC0000000u)) {
    m <<= 2;
    k += 2;
  }
  if (!(m & 0x80000000u)) k += 1;
  return k;
}

/* Multiplies two 16-bit values into 32 bits: one mulu instruction. */
static inline uint32_t mul16(uint32_t a, uint32_t b) {
  return (uint32_t)(uint16_t)a * (uint32_t)(uint16_t)b;
}

/* Rounds and packs a result. The value is (m / 2^31) * 2^(e - 127),
 * with bit 31 of m set; sticky is nonzero when bits below m were lost.
 * Gives the bits of the float, with infinity on overflow and a
 * subnormal number or zero on underflow. */
static uint32_t round_pack(uint32_t sign, int e, uint32_t m, uint32_t sticky) {
  uint32_t frac, rb;
  if (e >= 0xFF) return sign | INF;
  if (e <= 0) {
    /* A subnormal number: shift right, and keep what goes out as
     * sticky. */
    int shift = 1 - e;
    if (shift > 31) {
      sticky |= m;
      m = 0;
    } else {
      sticky |= m << (32 - shift);
      m >>= shift;
    }
    e = 0;
  }
  frac = m >> 8;
  rb = m & 0xFF;
  if (rb > 0x80 || (rb == 0x80 && (sticky || (frac & 1)))) frac++;
  /* frac has the implicit bit at bit 23 for a normal number. Adding it
   * to the exponent field makes a carry out of the fraction go into the
   * exponent, as it must. */
  if (e == 0) return sign | frac;
  return sign | (((uint32_t)(e - 1) << 23) + frac);
}

/* Gives the significand of a nonzero finite float of exponent field
 * *e and fraction frac, with the implicit bit at bit 23. A subnormal
 * number is normalized, and *e is made less than 1 to match. */
static inline uint32_t unpack_significand(int *e, uint32_t frac) {
  if (*e) return frac | IMPLICIT;
  {
    int k = clz32(frac) - 8;
    *e = 1 - k;
    return frac << k;
  }
}

/* The sum of two floats given by their bits. */
static uint32_t add_bits(uint32_t a, uint32_t b) {
  uint32_t aa = a & ~SIGN, ab = b & ~SIGN, sign, ma, mb, m;
  int ea, eb, d, k;
  if (aa >= INF || ab >= INF) {
    if (aa > INF) return a | QUIET;
    if (ab > INF) return b | QUIET;
    if (aa == INF) {
      /* inf - inf is invalid. */
      if (ab == INF && (a ^ b) & SIGN) return DEFAULT_NAN;
      return a;
    }
    return b;
  }
  if (aa < ab) {
    uint32_t t = a;
    a = b;
    b = t;
    t = aa;
    aa = ab;
    ab = t;
  }
  /* Now |a| >= |b|. */
  if (ab == 0) {
    /* x + 0 is x; 0 + 0 is +0 unless both are -0. */
    if (aa == 0) return a & b;
    return a;
  }
  sign = a & SIGN;
  ea = (int)(aa >> 23);
  eb = (int)(ab >> 23);
  /* The significands with seven more bits below, and room for a carry
   * at bit 31. A subnormal number has exponent 1 and no implicit bit. */
  ma = ea ? (aa & FRAC_MASK) | IMPLICIT : aa;
  mb = eb ? (ab & FRAC_MASK) | IMPLICIT : ab;
  if (!ea) ea = 1;
  if (!eb) eb = 1;
  ma <<= 7;
  mb <<= 7;
  d = ea - eb;
  if (d) {
    /* Align b; the lowest bit keeps what goes out (sticky). */
    if (d > 31)
      mb = 1;
    else
      mb = (mb >> d) | ((mb << (32 - d)) != 0);
  }
  if ((a ^ b) & SIGN) {
    m = ma - mb;
    if (m == 0) return 0; /* x - x is +0 */
  } else {
    m = ma + mb;
  }
  /* The implicit bit of a is at bit 30, for exponent ea. */
  k = clz32(m);
  return round_pack(sign, ea + 1 - k, m << k, 0);
}

/* Adds two floats. */
float SF_NAME(addsf3)(float a, float b) { return float_of(add_bits(bits_of(a), bits_of(b))); }

/* Subtracts the float b from the float a. */
float SF_NAME(subsf3)(float a, float b) { return float_of(add_bits(bits_of(a), bits_of(b) ^ SIGN)); }

/* Multiplies two floats. */
float SF_NAME(mulsf3)(float fa, float fb) {
  uint32_t a = bits_of(fa), b = bits_of(fb);
  uint32_t aa = a & ~SIGN, ab = b & ~SIGN, sign = (a ^ b) & SIGN;
  uint32_t ma, mb, a1, b1, p0, mid, lo, hi, m, sticky;
  int ea, eb, e;
  if (aa >= INF || ab >= INF || aa == 0 || ab == 0) {
    if (aa > INF) return float_of(a | QUIET);
    if (ab > INF) return float_of(b | QUIET);
    if (aa == INF) return float_of(ab ? sign | INF : DEFAULT_NAN);
    if (ab == INF) return float_of(aa ? sign | INF : DEFAULT_NAN);
    return float_of(sign);
  }
  ea = (int)(aa >> 23);
  eb = (int)(ab >> 23);
  ma = unpack_significand(&ea, aa & FRAC_MASK);
  mb = unpack_significand(&eb, ab & FRAC_MASK);
  /* The 48-bit product hi:lo of the two 24-bit significands, from four
   * 16-bit products. a1 and b1 have 8 bits. */
  a1 = ma >> 16;
  b1 = mb >> 16;
  p0 = mul16(ma, mb);
  mid = mul16(a1, mb) + mul16(ma, b1); /* less than 2^25 */
  lo = p0 + (mid << 16);
  hi = mul16(a1, b1) + (mid >> 16) + (lo < p0);
  /* The product is in [2^46, 2^48): hi has its bits 46 and 47 at 14
   * and 15. */
  e = ea + eb - 127;
  if (hi & 0x8000u) {
    m = (hi << 16) | (lo >> 16);
    sticky = lo & 0xFFFFu;
    e++;
  } else {
    m = (hi << 17) | (lo >> 15);
    sticky = lo & 0x7FFFu;
  }
  return float_of(round_pack(sign, e, m, sticky));
}

/* Divides the float a by the float b. */
float SF_NAME(divsf3)(float fa, float fb) {
  uint32_t a = bits_of(fa), b = bits_of(fb);
  uint32_t aa = a & ~SIGN, ab = b & ~SIGN, sign = (a ^ b) & SIGN;
  uint32_t ma, mb, q;
  int ea, eb, e, i;
  if (aa >= INF || ab >= INF || aa == 0 || ab == 0) {
    if (aa > INF) return float_of(a | QUIET);
    if (ab > INF) return float_of(b | QUIET);
    if (aa == INF) return float_of(ab == INF ? DEFAULT_NAN : sign | INF);
    if (ab == INF) return float_of(sign);
    if (ab == 0) return float_of(aa ? sign | INF : DEFAULT_NAN);
    return float_of(sign);
  }
  ea = (int)(aa >> 23);
  eb = (int)(ab >> 23);
  ma = unpack_significand(&ea, aa & FRAC_MASK);
  mb = unpack_significand(&eb, ab & FRAC_MASK);
  e = ea - eb + 127;
  if (ma < mb) {
    ma <<= 1;
    e--;
  }
  /* Now 1 <= ma / mb < 2. Find 26 bits of the quotient, one at a time:
   * the 68000's divu gives only 16. */
  q = 0;
  for (i = 0; i < 26; i++) {
    q <<= 1;
    if (ma >= mb) {
      ma -= mb;
      q |= 1;
    }
    ma <<= 1;
  }
  return float_of(round_pack(sign, e, q << 6, ma));
}

/* Compares two floats. Gives -1, 0 or 1 when a is less than, equal to
 * or greater than b, and unordered when either is a NaN. */
static inline int compare_bits(uint32_t a, uint32_t b, int unordered) {
  uint32_t aa = a & ~SIGN, ab = b & ~SIGN;
  if (aa > INF || ab > INF) return unordered;
  if ((aa | ab) == 0) return 0; /* +0 == -0 */
  if ((int32_t)(a & b) >= 0) {
    /* At least one is positive: compare as signed integers. */
    if ((int32_t)a < (int32_t)b) return -1;
    return a != b;
  }
  /* Both negative: the order of the bits is reversed. */
  if ((int32_t)a > (int32_t)b) return -1;
  return a != b;
}

/* Gives 0 when a == b, nonzero when not or when one is a NaN. */
int SF_NAME(eqsf2)(float a, float b) { return compare_bits(bits_of(a), bits_of(b), 1); }

/* Gives nonzero when a != b or when one is a NaN. */
int SF_NAME(nesf2)(float a, float b) { return compare_bits(bits_of(a), bits_of(b), 1); }

/* Gives a negative value when a < b; 1 when one is a NaN. */
int SF_NAME(ltsf2)(float a, float b) { return compare_bits(bits_of(a), bits_of(b), 1); }

/* Gives a value of 0 or less when a <= b; 1 when one is a NaN. */
int SF_NAME(lesf2)(float a, float b) { return compare_bits(bits_of(a), bits_of(b), 1); }

/* Gives a positive value when a > b; -1 when one is a NaN. */
int SF_NAME(gtsf2)(float a, float b) { return compare_bits(bits_of(a), bits_of(b), -1); }

/* Gives a value of 0 or more when a >= b; -1 when one is a NaN. */
int SF_NAME(gesf2)(float a, float b) { return compare_bits(bits_of(a), bits_of(b), -1); }

/* Gives nonzero when a or b is a NaN. */
int SF_NAME(unordsf2)(float a, float b) {
  return (bits_of(a) & ~SIGN) > INF || (bits_of(b) & ~SIGN) > INF;
}

/* Converts a float to an int, toward zero. A value out of range, or a
 * NaN, gives INT_MAX or INT_MIN by its sign, as compiler-rt does. */
int32_t SF_NAME(fixsfsi)(float fa) {
  uint32_t a = bits_of(fa), m;
  int e = (int)((a >> 23) & 0xFF) - 127;
  if (e < 0) return 0;
  if (e >= 31) {
    /* -2^31 is exact; all else is out of range. */
    return (a & SIGN) ? (int32_t)0x80000000u : 0x7FFFFFFF;
  }
  m = (a & FRAC_MASK) | IMPLICIT;
  m = e >= 23 ? m << (e - 23) : m >> (23 - e);
  return (a & SIGN) ? -(int32_t)m : (int32_t)m;
}

/* Converts an int to a float, rounded to nearest. */
float SF_NAME(floatsisf)(int32_t i) {
  uint32_t sign = 0, m;
  int k;
  if (i == 0) return float_of(0);
  m = (uint32_t)i;
  if (i < 0) {
    sign = SIGN;
    m = 0u - m;
  }
  k = clz32(m);
  /* The value is (m << k) / 2^31 * 2^(31 - k). */
  return float_of(round_pack(sign, 127 + 31 - k, m << k, 0));
}
