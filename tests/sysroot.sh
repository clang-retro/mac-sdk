#!/bin/sh
# Test a sysroot that build-sysroot.sh made: compile and link programs
# with the macapp.py of the sysroot only (not with this source tree), for
# 68k, PowerPC and fat, and run the 68k builds as a 68000 and a 68020 and
# the PowerPC builds in the CPU emulators of the sysroot.
#
#   CLANG=... tests/sysroot.sh SYSROOT
#
#   CLANG     clang-retro's clang (default: clang on the PATH)
#   SYSROOT   the sysroot to test
#
# The emulators need the unicorn Python package. Without it, the script
# only compiles and links. The C++ tests are done only when the sysroot
# has the C++ library.
set -e
tests=$(cd "$(dirname "$0")" && pwd)
root=$(cd "${1:?usage: tests/sysroot.sh SYSROOT}" && pwd)
CLANG=${CLANG:-clang}
export CLANG
work=$(mktemp -d "${TMPDIR:-/tmp}/sysroot-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
# The sources go to a directory of their own, so that nothing in this
# source tree can be found by mistake.
cp "$tests/libc.c" "$tests/thread.c" "$tests/net.c" "$tests/ot.c" \
  "$tests/cxx.cpp" "$tests/cxxthread.cpp" "$work/"
cd "$work"
macapp="python3 $root/tools/macapp.py"

# check NAME OUTPUT: stop when the output of a program shows a failure.
check() {
  if grep -q FAILED "$2" || ! grep -q "^all ok" "$2"; then
    echo "$1:"; cat "$2"; exit 1
  fi
}

# The examples, fat.
for ex in "$root"/examples/*.c; do
  $macapp -o "$(basename "$ex" .c).bin" -- -Wall -Werror "$ex" >/dev/null
done
echo "examples: linked"

progs="libc thread net ot"
[ -f "$root/lib/m68k/libstdc++.a" ] && progs="$progs cxx cxxthread"
for t in $progs; do
  src=$t.c
  [ -f "$src" ] || src=$t.cpp
  for cpu in 68k ppc; do
    $macapp --cpu $cpu -o "$t-$cpu.bin" -- -Wall -Werror "$src" >/dev/null
  done
done
echo "tests: linked ($progs)"

if ! python3 -c "import unicorn" 2>/dev/null; then
  echo "install unicorn to run the tests in the emulators"
  exit 0
fi
sim68k="python3 $root/tools/sim68k.py --os --files --exit-anywhere --steps 10000000"
simppc="python3 $root/tools/simppc.py --files"
for t in $progs; do
  input=
  [ $t = libc ] && input="typed\nline\n"
  for c in 68000 68020; do
    $sim68k --cpu $c --input "$input" "$t-68k.bin" >sim.txt || { cat sim.txt; exit 1; }
    check "$t on a $c" sim.txt
  done
  # Open Transport on PowerPC calls Mac OS's Open Transport, which the
  # emulator does not have (tools/qemu/nettest.py runs that build).
  [ $t = ot ] && continue
  $simppc --input "$input" "$t-ppc.bin" >sim.txt || { cat sim.txt; exit 1; }
  check "$t on PowerPC" sim.txt
done
echo "sysroot: ok"
