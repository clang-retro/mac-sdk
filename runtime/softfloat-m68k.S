// softfloat-m68k.S - single-precision soft float for the 68000.
//
// clang calls these functions for float arithmetic on a 68k without an
// FPU. They replace the ones of compiler-rt in libmacrt.a for 68k:
// __addsf3, __subsf3, __mulsf3, __divsf3, the comparisons (__eqsf2,
// __nesf2, __ltsf2, __lesf2, __gtsf2, __gesf2, __unordsf2), __fixsfsi
// and __floatsisf.
//
// The algorithms are those of runtime/softfloat.c, the reference, which
// tests/softfloat.c checks against the host's IEEE 754 arithmetic.
// tests/softfloat68k.c checks this file against vectors from the
// reference, in the simulator. The results are IEEE 754 binary32 with
// rounding to nearest, ties to even, with subnormal numbers.
//
// Calls: the arguments are on the stack (4(sp), 8(sp)), the result is in
// d0. d0, d1, a0 and a1 are scratch; the others are saved.
//
// Part of mac-sdk.
// SPDX-License-Identifier: AGPL-3.0-only

#define SIGN 0x80000000
#define INF 0x7F800000
#define QUIET 0x00400000
#define DEFAULT_NAN 0x7FC00000

// addq.l and subq.l, which the assembler does not take: n (1 to 8) to
// or from data register r.
.macro ADDQ_L n, r
	.short 0x5080 + ((\n & 7) << 9) + \r
.endm
.macro SUBQ_L n, r
	.short 0x5180 + ((\n & 7) << 9) + \r
.endm

	.text

// normalize: shifts d0 (not zero) left until bit 31 is set.
// Out: d0 shifted, d4 the number of places. Changes nothing else.
	.p2align 1
.Lnormalize:
	moveq #0, %d4
	cmpi.l #0x10000, %d0
	bcc 1f
	swap %d0		// the high half is zero: shift by 16
	moveq #16, %d4
1:	cmpi.l #0x1000000, %d0
	bcc 2f
	lsl.l #8, %d0
	ADDQ_L 8, 4
2:	move.l %d0, %d0
	bmi 4f
3:	ADDQ_L 1, 4
	add.l %d0, %d0
	bpl 3b
4:	rts

// roundpack: rounds and packs a result.
// In: d0 the significand m, with bit 31 set; d1 the biased exponent e
// (long, any value), so that the value is m / 2^31 * 2^(e - 127);
// d2 the sign (0 or SIGN); d3 nonzero when bits below m were lost.
// Out: d0 the bits of the float. Changes d1, d3 and d4.
	.p2align 1
.Lroundpack:
	cmpi.l #0xFF, %d1
	bge .Loverflow
	move.l %d1, %d1
	ble .Lsubnormal
	bsr.w .Lround
	SUBQ_L 1, 1		// (e - 1) << 23, plus frac with its implicit
	swap %d1		// bit: a carry out of the fraction goes into
	clr.w %d1		// the exponent
	lsl.l #7, %d1
	add.l %d1, %d0
	or.l %d2, %d0
	rts
.Loverflow:
	move.l %d2, %d0
	or.l #INF, %d0
	rts
.Lsubnormal:
	// shift = 1 - e; the bits that go out are sticky.
	neg.l %d1
	ADDQ_L 1, 1
	cmpi.l #31, %d1
	bgt 1f
	moveq #32, %d4
	sub.l %d1, %d4
	move.l %d0, -(%sp)
	lsl.l %d4, %d0		// the bits that go out
	or.l %d0, %d3
	move.l (%sp)+, %d0
	lsr.l %d1, %d0
	bra 2f
1:	or.l %d0, %d3
	moveq #0, %d0
2:	bsr.w .Lround
	or.l %d2, %d0		// exponent field 0 (or 1 after a carry)
	rts

// round: d0 = m >> 8, plus 1 when the low byte of m (and the sticky d3)
// say to round up, to nearest, ties to even. Changes d4.
.Lround:
	moveq #0, %d4
	move.b %d0, %d4
	lsr.l #8, %d0
	cmpi.w #0x80, %d4
	bhi 1f
	bne 2f
	move.l %d3, %d3
	bne 1f
	btst #0, %d0
	beq 2f
1:	ADDQ_L 1, 0
2:	rts

// float __subsf3(float a, float b): a - b.
	.p2align 1
	.globl __subsf3
	.type __subsf3,@function
__subsf3:
	movem.l %d2-%d7, -(%sp)
	move.l (28,%sp), %d0
	move.l (32,%sp), %d1
	bchg #31, %d1
	bra .Ladd
	.size __subsf3, .-__subsf3

// float __addsf3(float a, float b): a + b.
	.p2align 1
	.globl __addsf3
	.type __addsf3,@function
__addsf3:
	movem.l %d2-%d7, -(%sp)
	move.l (28,%sp), %d0
	move.l (32,%sp), %d1
.Ladd:
	move.l %d0, %d2
	bclr #31, %d2		// |a|
	move.l %d1, %d3
	bclr #31, %d3		// |b|
	move.l #INF, %d4
	cmp.l %d4, %d2
	bcc .Ladd_special_a
	cmp.l %d4, %d3
	bcc .Ladd_special_b
	cmp.l %d3, %d2
	bcc 1f
	move.l %d0, %d4		// make |a| >= |b|
	move.l %d1, %d0
	move.l %d4, %d1
	move.l %d2, %d4
	move.l %d3, %d2
	move.l %d4, %d3
1:	move.l %d3, %d3
	beq .Ladd_b_zero
	move.l %d0, %d7
	eor.l %d1, %d7		// bit 31: the signs differ
	and.l #SIGN, %d0
	move.l %d0, %a0		// the sign of the result
	// The exponents (words) and the significands, seven bits up.
	move.l %d2, %d5
	swap %d5
	lsr.w #7, %d5		// ea
	move.l %d3, %d6
	swap %d6
	lsr.w #7, %d6		// eb
	and.l #0x7FFFFF, %d2
	cmpi.w #0, %d5
	beq 2f
	bset #23, %d2
	bra 3f
2:	moveq #1, %d5		// subnormal: exponent 1, no implicit bit
3:	and.l #0x7FFFFF, %d3
	cmpi.w #0, %d6
	beq 4f
	bset #23, %d3
	bra 5f
4:	moveq #1, %d6
5:	lsl.l #7, %d2
	lsl.l #7, %d3
	ext.l %d5
	sub.w %d5, %d6
	neg.w %d6		// d = ea - eb, 0 or more
	beq .Ladd_aligned
	cmpi.w #31, %d6
	bgt 6f
	move.l %d3, %d0
	lsr.l %d6, %d3		// align b
	move.l %d3, %d1
	lsl.l %d6, %d1
	cmp.l %d1, %d0		// were bits lost?
	beq .Ladd_aligned
	moveq #1, %d0
	or.l %d0, %d3		// sticky
	bra .Ladd_aligned
6:	moveq #1, %d3		// all of b is sticky
.Ladd_aligned:
	move.l %d7, %d7
	bmi 7f
	add.l %d3, %d2
	bra 8f
7:	sub.l %d3, %d2
	beq .Ladd_zero
8:	move.l %d2, %d0
	bsr.w .Lnormalize		// the implicit bit of a was at bit 30
	move.l %d5, %d1
	ADDQ_L 1, 1
	sub.l %d4, %d1		// e = ea + 1 - k
	move.l %a0, %d2
	moveq #0, %d3
	bsr.w .Lroundpack
	movem.l (%sp)+, %d2-%d7
	rts
.Ladd_zero:
	moveq #0, %d0		// x - x is +0
	movem.l (%sp)+, %d2-%d7
	rts
.Ladd_b_zero:
	move.l %d2, %d2
	bne 1f
	and.l %d1, %d0		// 0 + 0: -0 only when both are -0
1:	movem.l (%sp)+, %d2-%d7
	rts
.Ladd_special_a:
	bne .Lquiet_a		// a is a NaN
	cmp.l %d4, %d3		// a is infinite
	bhi .Lquiet_b
	bne 1f
	move.l %d0, %d2
	eor.l %d1, %d2
	bpl 1f
	move.l #DEFAULT_NAN, %d0	// inf - inf
1:	movem.l (%sp)+, %d2-%d7
	rts
.Ladd_special_b:
	bne .Lquiet_b
	move.l %d1, %d0		// b is infinite, a finite
	movem.l (%sp)+, %d2-%d7
	rts
.Lquiet_a:
	or.l #QUIET, %d0
	movem.l (%sp)+, %d2-%d7
	rts
.Lquiet_b:
	move.l %d1, %d0
	or.l #QUIET, %d0
	movem.l (%sp)+, %d2-%d7
	rts
	.size __addsf3, .-__addsf3

// significand: d0 = the significand of a nonzero finite float whose
// magnitude is in d0, with the implicit bit at bit 23; d1 = its
// exponent field (long), less than 1 for a subnormal number, which is
// normalized. Changes d4.
	.p2align 1
.Lsignificand:
	move.l %d0, %d1
	swap %d1
	lsr.w #7, %d1
	and.l #0xFF, %d1
	beq 1f
	and.l #0x7FFFFF, %d0
	bset #23, %d0
	rts
1:	bsr.w .Lnormalize		// k zero bits: shift to bit 31, then back 8
	lsr.l #8, %d0
	moveq #9, %d1
	sub.l %d4, %d1		// e = 1 - (k - 8)
	rts

// float __mulsf3(float a, float b): a * b.
	.p2align 1
	.globl __mulsf3
	.type __mulsf3,@function
__mulsf3:
	movem.l %d2-%d7, -(%sp)
	move.l (28,%sp), %d0
	move.l (32,%sp), %d1
	move.l %d0, %d7
	eor.l %d1, %d7
	and.l #SIGN, %d7	// the sign of the result
	move.l %d0, %d2
	bclr #31, %d2
	move.l %d1, %d3
	bclr #31, %d3
	move.l #INF, %d4
	cmp.l %d4, %d2
	bcc .Lmul_special
	cmp.l %d4, %d3
	bcc .Lmul_special
	move.l %d2, %d2
	beq .Lmul_zero
	move.l %d3, %d3
	beq .Lmul_zero
	move.l %d2, %d0
	bsr.w .Lsignificand
	move.l %d0, %d2		// ma
	move.l %d1, %d5		// ea
	move.l %d3, %d0
	bsr.w .Lsignificand
	move.l %d0, %d3		// mb
	add.l %d1, %d5
	sub.l #126, %d5		// e = ea + eb - 127, +1 for a product >= 2
	// The 48-bit product hi:lo of the 24-bit significands, from four
	// 16-bit products (a1 and b1 have 8 bits).
	move.w %d2, %d4
	mulu %d3, %d4		// p0 = a0 * b0
	move.l %d2, %d6
	swap %d6
	mulu %d3, %d6		// a1 * b0
	move.l %d3, %d1
	swap %d1		// b1
	move.w %d1, %d0
	mulu %d2, %d0		// a0 * b1
	add.l %d0, %d6		// mid, less than 2^25
	swap %d2
	mulu %d1, %d2		// hi = a1 * b1
	move.l %d6, %d0
	swap %d0
	clr.w %d0		// mid << 16
	add.l %d0, %d4		// lo
	clr.w %d6
	swap %d6		// mid >> 16
	addx.l %d6, %d2		// hi, with the carry of lo
	btst #15, %d2		// bit 47 of the product
	bne 1f
	add.l %d4, %d4		// shift hi:lo left
	addx.l %d2, %d2
	SUBQ_L 1, 5
1:	swap %d2		// m = hi << 16 | lo >> 16 (hi has 16 bits)
	move.l %d4, %d0
	clr.w %d0
	swap %d0
	or.l %d2, %d0
	moveq #0, %d3
	move.w %d4, %d3		// sticky: the low 16 bits
	move.l %d5, %d1
	move.l %d7, %d2
	bsr.w .Lroundpack
	movem.l (%sp)+, %d2-%d7
	rts
.Lmul_zero:
	move.l %d7, %d0		// a signed zero
	movem.l (%sp)+, %d2-%d7
	rts
.Lmul_special:
	cmp.l %d4, %d2
	bhi .Lquiet_a
	cmp.l %d4, %d3
	bhi .Lquiet_b
	move.l %d2, %d2		// inf * 0 is invalid
	beq .Lnan
	move.l %d3, %d3
	beq .Lnan
	move.l %d7, %d0
	or.l %d4, %d0		// a signed infinity
	movem.l (%sp)+, %d2-%d7
	rts
.Lnan:
	move.l #DEFAULT_NAN, %d0
	movem.l (%sp)+, %d2-%d7
	rts
	.size __mulsf3, .-__mulsf3

// float __divsf3(float a, float b): a / b.
	.p2align 1
	.globl __divsf3
	.type __divsf3,@function
__divsf3:
	movem.l %d2-%d7, -(%sp)
	move.l (28,%sp), %d0
	move.l (32,%sp), %d1
	move.l %d0, %d7
	eor.l %d1, %d7
	and.l #SIGN, %d7
	move.l %d0, %d2
	bclr #31, %d2
	move.l %d1, %d3
	bclr #31, %d3
	move.l #INF, %d4
	cmp.l %d4, %d2
	bcc .Ldiv_special
	cmp.l %d4, %d3
	bcc .Ldiv_special
	move.l %d3, %d3
	beq .Ldiv_by_zero
	move.l %d2, %d2
	beq .Lmul_zero		// 0 / x: a signed zero
	move.l %d2, %d0
	bsr.w .Lsignificand
	move.l %d0, %d2		// ma
	move.l %d1, %d5		// ea
	move.l %d3, %d0
	bsr.w .Lsignificand
	move.l %d0, %d3		// mb
	sub.l %d1, %d5
	add.l #127, %d5		// e = ea - eb + 127
	cmp.l %d3, %d2
	bcc 1f
	add.l %d2, %d2
	SUBQ_L 1, 5
1:	// 1 <= ma / mb < 2: 26 bits of the quotient, one at a time.
	moveq #0, %d0
	moveq #25, %d1
2:	add.l %d0, %d0
	cmp.l %d3, %d2
	bcs 3f
	sub.l %d3, %d2
	ADDQ_L 1, 0
3:	add.l %d2, %d2
	sub.w #1, %d1
	bcc 2b
	lsl.l #6, %d0
	move.l %d2, %d3		// sticky: the remainder
	move.l %d5, %d1
	move.l %d7, %d2
	bsr.w .Lroundpack
	movem.l (%sp)+, %d2-%d7
	rts
.Ldiv_by_zero:
	move.l %d2, %d2
	beq .Lnan		// 0 / 0
	move.l %d7, %d0
	or.l %d4, %d0		// x / 0: a signed infinity
	movem.l (%sp)+, %d2-%d7
	rts
.Ldiv_special:
	cmp.l %d4, %d2
	bhi .Lquiet_a
	cmp.l %d4, %d3
	bhi .Lquiet_b
	cmp.l %d4, %d2
	bne .Lmul_zero		// x / inf: a signed zero
	cmp.l %d4, %d3
	beq .Lnan		// inf / inf
	move.l %d7, %d0
	or.l %d4, %d0		// inf / x
	movem.l (%sp)+, %d2-%d7
	rts
	.size __divsf3, .-__divsf3

// compare: d0 = -1, 0 or 1 when a (4(sp)) is less than, equal to or
// greater than b (8(sp)); d0 = d1 when either is a NaN. The functions
// below jump here; it returns to their caller. A float is a NaN when
// its bits, shifted left by one, are more than 0xFF000000.
	.p2align 1
.Lcompare:
	move.l %d2, -(%sp)
	move.l (8,%sp), %d0	// a
	move.l (12,%sp), %d2	// b
	move.l %d0, %a0
	move.l %d2, %a1
	add.l %d0, %d0		// |a| << 1
	cmpi.l #0xFF000000, %d0
	bhi 9f
	add.l %d2, %d2		// |b| << 1
	cmpi.l #0xFF000000, %d2
	bhi 9f
	or.l %d2, %d0
	beq 7f			// +0 == -0
	move.l %a0, %d0		// a
	move.l %a1, %d2		// b
	move.l %d0, %d1
	and.l %d2, %d1
	bmi 5f
	cmp.l %d2, %d0		// one is positive: compare as integers
	blt 6f
	bgt 4f
	bra 7f
5:	cmp.l %d2, %d0		// both negative: the order is reversed
	bgt 6f
	blt 4f
7:	moveq #0, %d0
	bra 8f
4:	moveq #1, %d0
	bra 8f
6:	moveq #-1, %d0
	bra 8f
9:	move.l %d1, %d0		// unordered
8:	move.l (%sp)+, %d2
	rts

// int __lesf2(float a, float b) and the others: see runtime/softfloat.c.
	.p2align 1
	.globl __lesf2
	.type __lesf2,@function
	.globl __eqsf2
	.type __eqsf2,@function
	.globl __nesf2
	.type __nesf2,@function
	.globl __ltsf2
	.type __ltsf2,@function
	.globl __cmpsf2
	.type __cmpsf2,@function
__lesf2:
__eqsf2:
__nesf2:
__ltsf2:
__cmpsf2:
	moveq #1, %d1		// a NaN gives 1
	bra .Lcompare
	.size __lesf2, .-__lesf2

	.p2align 1
	.globl __gesf2
	.type __gesf2,@function
	.globl __gtsf2
	.type __gtsf2,@function
__gesf2:
__gtsf2:
	moveq #-1, %d1		// a NaN gives -1
	bra .Lcompare
	.size __gesf2, .-__gesf2

// int __unordsf2(float a, float b): nonzero when a or b is a NaN. A
// float is a NaN when its bits, shifted left by one, are more than
// 0xFF000000.
	.p2align 1
	.globl __unordsf2
	.type __unordsf2,@function
__unordsf2:
	move.l (4,%sp), %d0
	add.l %d0, %d0
	cmpi.l #0xFF000000, %d0
	bhi 1f
	move.l (8,%sp), %d0
	add.l %d0, %d0
	cmpi.l #0xFF000000, %d0
	bhi 1f
	moveq #0, %d0
	rts
1:	moveq #1, %d0
	rts
	.size __unordsf2, .-__unordsf2

// int __fixsfsi(float a): a toward zero; INT_MAX or INT_MIN by the sign
// when out of range or a NaN.
	.p2align 1
	.globl __fixsfsi
	.type __fixsfsi,@function
__fixsfsi:
	move.l (4,%sp), %d0
	move.l %d0, %d1
	swap %d1
	lsr.w #7, %d1
	and.w #0xFF, %d1
	sub.w #127, %d1		// the unbiased exponent
	bmi 3f
	cmpi.w #31, %d1
	bge 4f
	move.l %d0, %a0		// keep the sign
	and.l #0x7FFFFF, %d0
	bset #23, %d0
	sub.w #23, %d1
	bmi 1f
	lsl.l %d1, %d0
	bra 2f
1:	neg.w %d1
	lsr.l %d1, %d0
2:	move.l %a0, %d1
	bpl 5f
	neg.l %d0
5:	rts
3:	moveq #0, %d0
	rts
4:	move.l %d0, %d0
	bmi 6f
	move.l #0x7FFFFFFF, %d0
	rts
6:	move.l #0x80000000, %d0
	rts
	.size __fixsfsi, .-__fixsfsi

// float __floatsisf(int i): i rounded to nearest.
	.p2align 1
	.globl __floatsisf
	.type __floatsisf,@function
__floatsisf:
	move.l (4,%sp), %d0
	beq 1f
	movem.l %d2-%d4, -(%sp)
	moveq #0, %d2
	move.l %d0, %d0
	bpl 2f
	move.l #SIGN, %d2
	neg.l %d0
2:	bsr.w .Lnormalize
	move.l #158, %d1	// 127 + 31 - k
	sub.l %d4, %d1
	moveq #0, %d3
	bsr.w .Lroundpack
	movem.l (%sp)+, %d2-%d4
1:	rts
	.size __floatsisf, .-__floatsisf

	.section .note.GNU-stack,"",@progbits
