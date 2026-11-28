#!/bin/sh
# Compile every header for both targets, in C and C++, and check record
# sizes (tests/sizes.c, and every size given in defs/), and check the
# argument-block calls against the ROM, and link the examples into
# applications. CLANG defaults to clang on the PATH. With LLVM_SRC set to
# a clang-retro/llvm checkout, also build and check the runtime library,
# and with PICOLIBC_SRC set to a clang-retro/picolibc checkout too, the C
# library, and with GCC_SRC set to an unpacked GCC 15.2.0 release too, the
# C++ library.
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
SIZES=$(mktemp "${TMPDIR:-/tmp}/defsizes.XXXXXX")
python3 tests/defsizes.py > "$SIZES"
INLINES=$(mktemp "${TMPDIR:-/tmp}/inlines.XXXXXX")
BLOCKS=$(mktemp "${TMPDIR:-/tmp}/blocks.XXXXXX")
trap 'rm -f "$SIZES" "$INLINES" "$BLOCKS"' EXIT
python3 tests/inlines.py > "$INLINES"
$CLANG --target=m68k-apple-macos -O1 -Iinclude -x c -c "$INLINES" -o /dev/null
# The Carbon accessors also have 68k glue, for code that opts in to them.
$CLANG --target=m68k-apple-macos -O1 -Iinclude -DTARGET_API_MAC_CARBON=1 -x c -c "$INLINES" -o /dev/null
# Argument-block calls against the ROM's glue (data/blockinfo.json).
python3 tests/blocks.py > "$BLOCKS"
$CLANG --target=m68k-apple-macos -Wall -Werror -Wno-unused-function -Iinclude -x c -fsyntax-only "$BLOCKS"
for t in m68k-apple-macos powerpc-apple-macos9; do
  $CLANG --target=$t -fsyntax-only -Wall -Werror -Iinclude tests/all.c
  # The pre-1993 names (GetCtlValue, DisposPtr, ...) that old code uses.
  $CLANG --target=$t -fsyntax-only -Wall -Werror -DOLDROUTINENAMES=1 -Iinclude tests/all.c
  # The headers themselves must not trip the opt-in availability warnings,
  # and a call newer than MAC_MIN_SYSTEM must.
  $CLANG --target=$t -fsyntax-only -Wall -Werror -DMAC_MIN_SYSTEM=0x0010 -DMAC_NO_COLOR_QD -Iinclude tests/all.c
  $CLANG --target=$t -fsyntax-only -DMAC_MIN_SYSTEM=0x0603 -Iinclude examples/hello.c 2>&1 |
    grep -q "needs System 6.0.4 or later"
  $CLANG --target=$t -fsyntax-only -Iinclude tests/sizes.c
  $CLANG --target=$t -fsyntax-only -Iinclude -x c "$SIZES"
  $CLANG --target=$t -x c++ -std=c++17 -fsyntax-only -Werror -Iinclude tests/all.c
  for ex in examples/*.c; do
    $CLANG --target=$t -O2 -Wall -Werror -Iinclude -c "$ex" -o /dev/null
  done
  echo "$t: ok"
done
# Link every example into a fat application (68k CODE and PowerPC PEF).
APPS=$(mktemp -d "${TMPDIR:-/tmp}/apps.XXXXXX")
trap 'rm -rf "$SIZES" "$INLINES" "$BLOCKS" "$APPS"' EXIT
for ex in examples/*.c; do
  CLANG="$CLANG" python3 tools/macapp.py -o "$APPS/$(basename "$ex" .c).bin" \
    -- -Wall -Werror "$ex" >/dev/null
done
echo "examples: linked"
# A 68k code resource and a faceless background application: the file
# types, the resource and the SIZE flags.
CLANG="$CLANG" python3 tools/macapp.py --cpu 68k --no-libc --no-runtime \
  --code-resource 'TEST,128' --type TeSt \
  -o "$APPS/coderes.bin" -- -Wall -Werror tests/coderes.c >/dev/null
CLANG="$CLANG" python3 tools/macapp.py --type appe --background-only \
  -o "$APPS/bg.bin" -- -Wall -Werror examples/hello.c >/dev/null
python3 - "$APPS/coderes.bin" "$APPS/bg.bin" <<'PY'
import sys
sys.path.insert(0, "tools/link")
from macres import read_macbinary, read_resource_fork
_, ftype, _, data, rsrc = read_macbinary(open(sys.argv[1], "rb").read())
res = read_resource_fork(rsrc)
assert ftype == b"TeSt" and not data, ftype
assert [(t, i) for t, i, _ in res] == [(b"TEST", 128)], res
# start-coderes.S comes first: movem.l d0-d2/a0-a1,-(sp)
assert res[0][2][:4] == bytes.fromhex("48e7e0c0"), res[0][2][:4].hex()
_, ftype, _, _, rsrc = read_macbinary(open(sys.argv[2], "rb").read())
size = [b for t, _, b in read_resource_fork(rsrc) if t == b"SIZE"][0]
assert ftype == b"appe" and size[0] & 0x04, (ftype, size.hex())
PY
echo "code resource, background application: linked"
# The runtime library, when LLVM_SRC names a clang-retro/llvm checkout (for
# compiler-rt): build it, link tests/runtime.c for both CPUs, and run the
# 68k build as a 68000 and a 68020 in tools/sim68k.py (with unicorn).
# tests/fixmath.c and tests/glue.c do the same for the 68k glue.
if [ -n "$LLVM_SRC" ]; then
  CLANG="$CLANG" LLVM_SRC="$LLVM_SRC" runtime/build.sh "$APPS/lib"
  for t in runtime fixmath glue; do
    for cpu in 68k ppc; do
      CLANG="$CLANG" python3 tools/macapp.py --cpu $cpu --lib "$APPS/lib" \
        -o "$APPS/$t-$cpu.bin" -- -Wall -Werror tests/$t.c >/dev/null
    done
  done
  if python3 -c "import unicorn" 2>/dev/null; then
    for t in runtime fixmath glue; do
      for c in 68000 68020; do
        python3 tools/sim68k.py --cpu $c --os "$APPS/$t-68k.bin" >"$APPS/sim.txt" || {
          cat "$APPS/sim.txt"; exit 1; }
        grep -q "DrawString('all ok')" "$APPS/sim.txt" || {
          echo "$t on a $c:"; grep DrawString "$APPS/sim.txt" | grep -v "'ok'"; exit 1; }
      done
    done
    # The PowerPC builds in tools/simppc.py (not fixmath: InterfaceLib
    # has the FixMath calls there, and the simulator does not fake them).
    for t in runtime glue; do
      python3 tools/simppc.py "$APPS/$t-ppc.bin" >"$APPS/sim.txt" || {
        cat "$APPS/sim.txt"; exit 1; }
      grep -q "DrawString('all ok')" "$APPS/sim.txt" || {
        echo "$t on PowerPC:"; grep DrawString "$APPS/sim.txt" | grep -v "'ok'"; exit 1; }
    done
    echo "runtime: ok"
  else
    echo "runtime: linked (install unicorn to run it)"
  fi
fi
# The C library, when PICOLIBC_SRC names a clang-retro/picolibc checkout
# too: build it, link tests/libc.c for both CPUs, and run the 68k build in
# tools/sim68k.py with its Memory and File Managers, and the PowerPC one
# in tools/simppc.py, checking the console output file each leaves, and tests/console.c
# with the console window, typing into it, tests/thread.c with the
# simulators' fake Thread Manager, tests/net.c with their fake MacTCP
# and PPC Toolbox, tests/ot.c (68k) with the fake MacTCP, and tests/gl.c with the PowerPC simulator's fake
# OpenGL.
if [ -n "$LLVM_SRC" ] && [ -n "$PICOLIBC_SRC" ]; then
  CLANG="$CLANG" PICOLIBC_SRC="$PICOLIBC_SRC" runtime/build-libc.sh "$APPS/lib"
  for cpu in 68k ppc; do
    CLANG="$CLANG" python3 tools/macapp.py --cpu $cpu --lib "$APPS/lib" \
      -o "$APPS/libc-$cpu.bin" -- -Wall -Werror tests/libc.c >/dev/null
    CLANG="$CLANG" python3 tools/macapp.py --cpu $cpu --lib "$APPS/lib" \
      --console window -o "$APPS/console-$cpu.bin" -- -Wall -Werror \
      tests/console.c >/dev/null
    CLANG="$CLANG" python3 tools/macapp.py --cpu $cpu --lib "$APPS/lib" \
      -o "$APPS/thread-$cpu.bin" -- -Wall -Werror tests/thread.c >/dev/null
    CLANG="$CLANG" python3 tools/macapp.py --cpu $cpu --lib "$APPS/lib" \
      -o "$APPS/net-$cpu.bin" -- -Wall -Werror tests/net.c >/dev/null
    CLANG="$CLANG" python3 tools/macapp.py --cpu $cpu --lib "$APPS/lib" \
      -o "$APPS/ot-$cpu.bin" -- -Wall -Werror tests/ot.c >/dev/null
    # The examples that need the C library.
    for ex in examples/net/*.c; do
      CLANG="$CLANG" python3 tools/macapp.py --cpu $cpu --lib "$APPS/lib" \
        --console window -o "$APPS/$(basename "$ex" .c)-$cpu.bin" -- -Wall -Werror "$ex" >/dev/null
    done
  done
  # OpenGL is PowerPC only.
  CLANG="$CLANG" python3 tools/macapp.py --cpu ppc --lib "$APPS/lib" \
    -o "$APPS/gl-ppc.bin" -- -Wall -Werror tests/gl.c >/dev/null
  if python3 -c "import unicorn" 2>/dev/null; then
    for c in 68000 68020; do
      python3 tools/sim68k.py --cpu $c --os --files --exit-anywhere --input "typed\nline\n" \
        "$APPS/libc-68k.bin" >"$APPS/sim.txt" || { cat "$APPS/sim.txt"; exit 1; }
      if grep -q FAILED "$APPS/sim.txt" || ! grep -q "^all ok" "$APPS/sim.txt"; then
        echo "libc on a $c:"; cat "$APPS/sim.txt"; exit 1
      fi
    done
    python3 tools/simppc.py --files --input "typed\nline\n" \
      "$APPS/libc-ppc.bin" >"$APPS/sim.txt" || { cat "$APPS/sim.txt"; exit 1; }
    if grep -q FAILED "$APPS/sim.txt" || ! grep -q "^all ok" "$APPS/sim.txt"; then
      echo "libc on PowerPC:"; cat "$APPS/sim.txt"; exit 1
    fi
    # The threads, over the simulators' fake Thread Manager.
    for c in 68000 68020; do
      python3 tools/sim68k.py --cpu $c --os --files --exit-anywhere \
        "$APPS/thread-68k.bin" >"$APPS/sim.txt" || { cat "$APPS/sim.txt"; exit 1; }
      if grep -q FAILED "$APPS/sim.txt" || ! grep -q "^all ok" "$APPS/sim.txt"; then
        echo "threads on a $c:"; cat "$APPS/sim.txt"; exit 1
      fi
    done
    python3 tools/simppc.py --files "$APPS/thread-ppc.bin" >"$APPS/sim.txt" ||
      { cat "$APPS/sim.txt"; exit 1; }
    if grep -q FAILED "$APPS/sim.txt" || ! grep -q "^all ok" "$APPS/sim.txt"; then
      echo "threads on PowerPC:"; cat "$APPS/sim.txt"; exit 1
    fi
    # Pipes and sockets, over the simulators' fake MacTCP (the host's
    # loopback network) and fake PPC Toolbox.
    for c in 68000 68020; do
      python3 tools/sim68k.py --cpu $c --os --files --exit-anywhere \
        "$APPS/net-68k.bin" >"$APPS/sim.txt" || { cat "$APPS/sim.txt"; exit 1; }
      if grep -q FAILED "$APPS/sim.txt" || ! grep -q "^all ok" "$APPS/sim.txt"; then
        echo "sockets on a $c:"; cat "$APPS/sim.txt"; exit 1
      fi
    done
    python3 tools/simppc.py --files "$APPS/net-ppc.bin" >"$APPS/sim.txt" ||
      { cat "$APPS/sim.txt"; exit 1; }
    if grep -q FAILED "$APPS/sim.txt" || ! grep -q "^all ok" "$APPS/sim.txt"; then
      echo "sockets on PowerPC:"; cat "$APPS/sim.txt"; exit 1
    fi
    # Open Transport on 68k (over MacTCP), over the fake MacTCP. On
    # PowerPC the calls go to Mac OS's Open Transport, which the
    # simulator does not have: tools/qemu/nettest.py runs that build.
    for c in 68000 68020; do
      python3 tools/sim68k.py --cpu $c --os --files --exit-anywhere \
        "$APPS/ot-68k.bin" >"$APPS/sim.txt" || { cat "$APPS/sim.txt"; exit 1; }
      if grep -q FAILED "$APPS/sim.txt" || ! grep -q "^all ok" "$APPS/sim.txt"; then
        echo "Open Transport on a $c:"; cat "$APPS/sim.txt"; exit 1
      fi
    done
    # OpenGL and AGL, over the PowerPC simulator's fake OpenGL.
    python3 tools/simppc.py --files "$APPS/gl-ppc.bin" >"$APPS/sim.txt" ||
      { cat "$APPS/sim.txt"; exit 1; }
    if grep -q FAILED "$APPS/sim.txt" || ! grep -q "^all ok" "$APPS/sim.txt"; then
      echo "OpenGL on PowerPC:"; cat "$APPS/sim.txt"; exit 1
    fi
    # The console window: typed keys in, the text it draws out.
    for c in 68000 68020; do
      python3 tools/sim68k.py --cpu $c --os --exit-anywhere --keys "Ada\n" \
        "$APPS/console-68k.bin" >"$APPS/sim.txt" 2>&1 || { cat "$APPS/sim.txt"; exit 1; }
      grep -q "DrawText('console ok')" "$APPS/sim.txt" ||
        { echo "console window on a $c:"; cat "$APPS/sim.txt"; exit 1; }
    done
    python3 tools/simppc.py --keys "Ada\n" "$APPS/console-ppc.bin" \
      >"$APPS/sim.txt" 2>&1 || { cat "$APPS/sim.txt"; exit 1; }
    grep -q "DrawText('console ok')" "$APPS/sim.txt" ||
      { echo "console window on PowerPC:"; cat "$APPS/sim.txt"; exit 1; }
    echo "libc: ok"
  else
    echo "libc: linked (install unicorn to run it)"
  fi
fi
# The soft float of the 68k runtime: test the reference, runtime/softfloat.c,
# against the host's IEEE 754 arithmetic, write vectors from it, and check
# runtime/softfloat-m68k.S (in libmacrt.a) against them as a 68000 and a
# 68020.
if [ -n "$LLVM_SRC" ] && [ -n "$PICOLIBC_SRC" ]; then
  ${HOSTCC:-cc} -O2 -ffp-contract=off -o "$APPS/softfloat" tests/softfloat.c
  "$APPS/softfloat" 2000000
  "$APPS/softfloat" --vectors 3000 >"$APPS/softfloat-vectors.h"
  CLANG="$CLANG" python3 tools/macapp.py --cpu 68k --lib "$APPS/lib" \
    -o "$APPS/softfloat-68k.bin" -- -Wall -Werror -I"$APPS" \
    tests/softfloat68k.c >/dev/null
  if python3 -c "import unicorn" 2>/dev/null; then
    for c in 68000 68020; do
      python3 tools/sim68k.py --cpu $c --os --files --exit-anywhere \
        --mem 8 --steps 2000000000 "$APPS/softfloat-68k.bin" >"$APPS/sim.txt" ||
        { cat "$APPS/sim.txt"; exit 1; }
      grep -q "^all ok" "$APPS/sim.txt" ||
        { echo "softfloat on a $c:"; cat "$APPS/sim.txt"; exit 1; }
    done
    echo "softfloat: ok"
  else
    echo "softfloat: linked (install unicorn to run it)"
  fi
fi
# The C++ library, when GCC_SRC names an unpacked GCC 15.2.0 release too:
# build libstdc++ and libunwind, link tests/cxx.cpp and tests/cxxthread.cpp
# for both CPUs, and run the 68k builds as a 68000 and a 68020 and the
# PowerPC builds in the simulators, checking the console output that each
# leaves.
if [ -n "$LLVM_SRC" ] && [ -n "$PICOLIBC_SRC" ] && [ -n "$GCC_SRC" ]; then
  CLANG="$CLANG" LLVM_SRC="$LLVM_SRC" GCC_SRC="$GCC_SRC" \
    runtime/build-libstdcxx.sh "$APPS/lib" >"$APPS/cxxbuild.txt" 2>&1 || {
    tail -50 "$APPS/cxxbuild.txt"; exit 1; }
  for t in cxx cxxthread; do
    for cpu in 68k ppc; do
      CLANG="$CLANG" python3 tools/macapp.py --cpu $cpu --lib "$APPS/lib" \
        -o "$APPS/$t-$cpu.bin" -- -Wall -Werror tests/$t.cpp >/dev/null
    done
  done
  if python3 -c "import unicorn" 2>/dev/null; then
    for t in cxx cxxthread; do
      for c in 68000 68020; do
        python3 tools/sim68k.py --cpu $c --os --files --exit-anywhere \
          "$APPS/$t-68k.bin" >"$APPS/sim.txt" || { cat "$APPS/sim.txt"; exit 1; }
        if grep -q FAILED "$APPS/sim.txt" || ! grep -q "^all ok" "$APPS/sim.txt"; then
          echo "$t on a $c:"; cat "$APPS/sim.txt"; exit 1
        fi
      done
      python3 tools/simppc.py --files "$APPS/$t-ppc.bin" >"$APPS/sim.txt" || {
        cat "$APPS/sim.txt"; exit 1; }
      if grep -q FAILED "$APPS/sim.txt" || ! grep -q "^all ok" "$APPS/sim.txt"; then
        echo "$t on PowerPC:"; cat "$APPS/sim.txt"; exit 1
      fi
    done
    echo "c++: ok"
  else
    echo "c++: linked (install unicorn to run it)"
  fi
fi
# Quesa (QuickDraw 3D over OpenGL), when QUESA_SRC names a checkout of
# jwwalker/Quesa at the commit that extras/quesa/build.sh names, too:
# build it for PowerPC, link tests/quesa.cpp, and run it in the PowerPC
# simulator over the fake OpenGL.
if [ -n "$LLVM_SRC" ] && [ -n "$PICOLIBC_SRC" ] && [ -n "$GCC_SRC" ] &&
   [ -n "$QUESA_SRC" ]; then
  CLANG="$CLANG" extras/quesa/build.sh "$QUESA_SRC" "$APPS/lib" >/dev/null
  CLANG="$CLANG" python3 tools/macapp.py --cpu ppc --lib "$APPS/lib" \
    -o "$APPS/quesa-ppc.bin" -- -Wall -Werror -Wno-pragma-pack \
    -I"$APPS/lib/include/Quesa" tests/quesa.cpp "$APPS/lib/ppc/libquesa.a" \
    >/dev/null
  if python3 -c "import unicorn" 2>/dev/null; then
    python3 tools/simppc.py --files "$APPS/quesa-ppc.bin" >"$APPS/sim.txt" || {
      cat "$APPS/sim.txt"; exit 1; }
    if grep -q FAILED "$APPS/sim.txt" || ! grep -q "^all ok" "$APPS/sim.txt"; then
      echo "quesa on PowerPC:"; cat "$APPS/sim.txt"; exit 1
    fi
    echo "quesa: ok"
  else
    echo "quesa: linked (install unicorn to run it)"
  fi
fi
