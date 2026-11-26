# Soft float on the 68000

A 68k Mac without an FPU (all 68000 Macs, and the 68020 and 68030 Macs
without a 68881 or 68882) does floating-point operations in software.
clang calls a helper function in `libmacrt.a` for each `float` or
`double` operation.

The `float` helpers in `libmacrt.a` for 68k are in 68000 assembly, in
`runtime/softfloat-m68k.S`. They replace the helpers of compiler-rt.
The helpers of compiler-rt are in C. They use `__muldi3` and
`__mulsi3` for each multiply, and the 68000 has only a 16-bit multiply
instruction.

## The functions

| Function | Operation |
| --- | --- |
| `__addsf3`, `__subsf3` | `a + b`, `a - b` |
| `__mulsf3` | `a * b` |
| `__divsf3` | `a / b` |
| `__eqsf2`, `__nesf2`, `__ltsf2`, `__lesf2`, `__cmpsf2` | Compare. The result is 1 when an operand is a NaN. |
| `__gtsf2`, `__gesf2` | Compare. The result is -1 when an operand is a NaN. |
| `__unordsf2` | Not zero when an operand is a NaN. |
| `__fixsfsi` | `(int)a`. Values out of range give `INT_MAX` or `INT_MIN`. |
| `__floatsisf` | `(float)i` |

The `double` helpers and the other conversions are the helpers of
compiler-rt.

## The results

The results are IEEE 754 binary32:

- Rounding is to nearest, ties to even.
- Subnormal numbers are supported. They are not set to zero.
- A NaN operand gives the same NaN, with the quiet bit set. When the
  two operands are NaNs, the result is the first operand.
- An invalid operation (for example `inf - inf` or `0 / 0`) gives the
  default NaN, `0x7FC00000`.
- A NaN given to `__fixsfsi` gives `INT_MAX` or `INT_MIN`, by its sign
  bit.

The functions set no exception flags.

## Speed

These values are for one operation on a 68000, with operands that
change. They include the call. The cycle values are estimates from
the simulator: 4 cycles for each bus word, the cost of the shift and
multiply instructions, and no wait states.

| Operation | compiler-rt (cycles) | `softfloat-m68k.S` (cycles) | Speedup |
| --- | --- | --- | --- |
| `a + b` | 910 | 709 | 1.3 times |
| `a - b` | 1216 | 725 | 1.7 times |
| `a * b` | 2107 | 939 | 2.2 times |
| `a / b` | 10471 | 1330 | 7.9 times |
| `a < b` | 278 | 247 | 1.1 times |
| `(int)a` | 670 | 156 | 4.3 times |
| `(float)i` | 687 | 382 | 1.8 times |

The code of all the functions is in one object of 1134 bytes.
A program that uses one of the functions links all of them.

## The reference and the tests

`runtime/softfloat.c` is the reference. It has the same algorithms in
C. `libmacrt.a` does not use it.

- `tests/softfloat.c` compiles the reference on the host. It compares
  the reference with the IEEE 754 arithmetic of the host for random
  operands and special values (zeros, subnormal numbers, infinities,
  NaNs and halfway cases). The argument is the number of operand
  pairs.
- `tests/softfloat.c --vectors N` writes `softfloat-vectors.h`: the
  special values and `N` random pairs for each function, with the
  results of the reference.
- `tests/softfloat68k.c` runs each vector through the functions of
  `libmacrt.a` and compares the results. A NaN result agrees with any
  NaN.

`tests/run.sh` does these steps when `LLVM_SRC` and `PICOLIBC_SRC` are
set. It runs `tests/softfloat68k.c` as a 68000 and as a 68020 in
`tools/sim68k.py`. To run the host test alone:

```sh
cc -O2 -ffp-contract=off -o softfloat tests/softfloat.c
./softfloat 20000000
```

The output is `softfloat: ok (0 failures)`.

## Changes to the assembly

Change `runtime/softfloat.c` first, and make sure that the host test
passes. Then make the same change in `runtime/softfloat-m68k.S`, and
run `tests/run.sh`.

The M68k assembler of LLVM does not accept some instructions:

- `tst`: use `move.l %dN, %dN`.
- `addq.l` and `subq.l`: use the `ADDQ_L` and `SUBQ_L` macros at the
  top of the file.
- `cmp.l #n`: use `cmpi.l #n`.
- `bsr.s`: use `bsr.w`.
- `exg` and `dbra`.
