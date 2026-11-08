#!/bin/sh
# Build the C library for 68k and PowerPC: picolibc (stdio, stdlib,
# string, math, ...) and the Mac layer under it in runtime/libc/ (files
# over the File Manager, console output, malloc over the Memory Manager,
# time, startup and exit).
#
#   CLANG=... PICOLIBC_SRC=... runtime/build-libc.sh [OUT]
#
#   CLANG         clang-retro's clang (default: clang on the PATH); llvm-ar
#                 is taken from the same directory
#   PICOLIBC_SRC  a clang-retro/picolibc checkout
#   OUT           where to put include/ (picolibc's headers) and
#                 m68k/ and ppc/ (libc.a, libmacos.a) (default: lib/ in
#                 the SDK, where tools/macapp.py looks)
#
# Needs meson and ninja. Build the runtime library (runtime/build.sh) too:
# picolibc calls the compiler's helpers.
set -e
here=$(cd "$(dirname "$0")" && pwd)
sdk=$(dirname "$here")
: "${PICOLIBC_SRC:?set PICOLIBC_SRC to a clang-retro/picolibc checkout}"
CLANG=${CLANG:-clang}
clang=$(command -v "$CLANG")
bindir=$(dirname "$clang")
out=${1:-$sdk/lib}
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/maclibc.XXXXXX")
trap 'rm -rf "$work"' EXIT

for cpu in m68k ppc; do
  case $cpu in
    m68k) target=m68k-apple-macos; family=m68k; cpuname=68000; flags="'-mcpu=68000', " ;;
    ppc) target=powerpc-apple-macos9; family=powerpc; cpuname=ppc; flags="" ;;
  esac
  cat > "$work/cross-$cpu.txt" <<EOF
[binaries]
c = ['$clang', '--target=$target', $flags'-nostdlib']
ar = '$bindir/llvm-ar'
nm = '$bindir/llvm-nm'
strip = '$bindir/llvm-strip'

[host_machine]
system = 'none'
cpu_family = '$family'
cpu = '$cpuname'
endian = 'big'

[properties]
skip_sanity_check = true
EOF
  # No debug information: clang cannot write it for XCOFF yet.
  meson setup "$work/build-$cpu" "$PICOLIBC_SRC" \
    --cross-file "$work/cross-$cpu.txt" --prefix="$work/install-$cpu" \
    -Ddebug=false -Doptimization=s -Dmultilib=false -Dpicocrt=false \
    -Dsemihost=false -Dtests=false -Dsingle-thread=true \
    -Dthread-local-storage=false -Dspecsdir=none -Dposix-console=true \
    -Denable-malloc=false -Dnewlib-global-errno=true \
    >"$work/setup-$cpu.log" 2>&1 || { tail -20 "$work/setup-$cpu.log"; exit 1; }
  ninja -C "$work/build-$cpu" install >"$work/build-$cpu.log" 2>&1 ||
    { grep -E '^FAILED|error' "$work/build-$cpu.log" | head -20; exit 1; }
  mkdir -p "$out/$cpu"
  cp "$work/install-$cpu/lib/libc.a" "$out/$cpu/libc.a"
  # The headers are the same for both CPUs.
  if [ $cpu = m68k ]; then
    rm -rf "$out/include"
    cp -R "$work/install-$cpu/include" "$out/include"
  fi

  # The Mac layer, one archive member per file.
  mkdir -p "$work/macos-$cpu"
  for f in "$here"/libc/*.c; do
    "$CLANG" --target=$target -O2 -ffreestanding -Wall -Wextra -Werror \
      -isystem "$out/include" -I "$sdk/include" \
      -c "$f" -o "$work/macos-$cpu/$(basename "$f" .c).o"
  done
  rm -f "$out/$cpu/libmacos.a"
  "$bindir/llvm-ar" rcs "$out/$cpu/libmacos.a" "$work/macos-$cpu"/*.o
  echo "$cpu: $out/$cpu/libc.a and libmacos.a"
done
