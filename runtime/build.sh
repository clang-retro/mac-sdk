#!/bin/sh
# Build the runtime library, libmacrt.a, for 68k and PowerPC: compiler-rt's
# builtins (32-bit multiply and divide for the 68000, 64-bit arithmetic,
# soft float on 68k, conversions) and the memory and string functions in
# string.c.
#
#   CLANG=... LLVM_SRC=... runtime/build.sh [OUT]
#
#   CLANG     clang-retro's clang (default: clang on the PATH); llvm-ar is
#             taken from the same directory
#   LLVM_SRC  a clang-retro/llvm checkout (only compiler-rt/lib/builtins
#             is read)
#   OUT       where to put m68k/libmacrt.a and ppc/libmacrt.a (default:
#             lib/ in the SDK, where tools/macapp.py looks)
#
# Each builtin is its own archive member, so a program links only those
# it calls. The archives are in the GNU ar format, which codelink.py and
# peflink.py read.
set -e
here=$(cd "$(dirname "$0")" && pwd)
sdk=$(dirname "$here")
: "${LLVM_SRC:?set LLVM_SRC to a clang-retro/llvm checkout}"
CLANG=${CLANG:-clang}
out=${1:-$sdk/lib}
builtins=$LLVM_SRC/compiler-rt/lib/builtins
[ -f "$builtins/int_lib.h" ] || { echo "no compiler-rt builtins in $LLVM_SRC" >&2; exit 1; }
bindir=$(dirname "$(command -v "$CLANG")")
AR=${AR:-$bindir/llvm-ar}
work=$(mktemp -d "${TMPDIR:-/tmp}/macrt.XXXXXX")
trap 'rm -rf "$work"' EXIT

# Files that need an operating system, or a type these targets lack.
skip=" atomic crtbegin crtend emutls enable_execute_stack eprintf clear_cache
  gcc_personality_v0 trampoline_setup os_version_check cpu_model
  truncdfbf2 truncsfbf2 extendhfxf2 truncxfhf2 "

for cpu in m68k ppc; do
  case $cpu in
    m68k) target=m68k-apple-macos; flags="-mcpu=68000" ;;
    # The PowerPC target makes XCOFF, as AIX does; compiler-rt knows how
    # to make aliases there only for AIX.
    ppc) target=powerpc-apple-macos9; flags="-D_AIX" ;;
  esac
  mkdir -p "$work/$cpu" "$out/$cpu"
  cc="$CLANG --target=$target -O2 -ffreestanding -fno-builtin $flags"
  for f in "$builtins"/*.c; do
    n=$(basename "$f" .c)
    case $skip in *" $n "*) continue ;; esac
    # Some builtins are for types or CPUs these targets lack (__int128,
    # x87 long double); they fail to compile and are left out.
    $cc -c "$f" -o "$work/$cpu/$n.o" 2>/dev/null || rm -f "$work/$cpu/$n.o"
  done
  # The 68000's 32-bit multiply is in assembly.
  if [ $cpu = m68k ]; then
    for f in "$builtins"/m68k/*.S; do
      $cc -c "$f" -o "$work/$cpu/$(basename "$f" .S).o"
    done
  fi
  # The ones clang calls for plain C must be there.
  need="divdi3 udivdi3 moddi3 umoddi3 fixdfdi floatdidf fixunsdfsi"
  [ $cpu = m68k ] && need="$need mulsi3 divsi3 udivsi3 modsi3 umodsi3 muldi3
    addsf3 subsf3 mulsf3 divsf3 adddf3 subdf3 muldf3 divdf3 comparesf2
    comparedf2 fixdfsi floatsidf fixsfsi floatsisf extendsfdf2 truncdfsf2"
  for n in $need; do
    [ -f "$work/$cpu/$n.o" ] || { echo "$cpu: $n.c did not compile" >&2; exit 1; }
  done
  $cc -Wall -Wextra -Werror -c "$here/string.c" -o "$work/$cpu/string.o"
  rm -f "$out/$cpu/libmacrt.a"
  "$AR" --format=gnu rcs "$out/$cpu/libmacrt.a" "$work/$cpu"/*.o
  echo "$cpu: $(ls "$work/$cpu" | wc -l) objects in $out/$cpu/libmacrt.a"
done
