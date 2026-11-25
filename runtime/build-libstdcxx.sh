#!/bin/sh
# Build the C++ runtime for 68k and PowerPC: LLVM's unwinder (libunwind.a)
# and GCC's C++ library (libstdc++.a and libsupc++.a), compiled by clang
# over the C library of runtime/build-libc.sh.
#
#   CLANG=... LLVM_SRC=... GCC_SRC=... runtime/build-libstdcxx.sh [OUT]
#
#   CLANG     clang-retro's clang (default: clang on the PATH); clang++,
#             llvm-ar, llvm-ranlib and llvm-nm are taken from the same
#             directory
#   LLVM_SRC  a clang-retro/llvm checkout (only libunwind is read)
#   GCC_SRC   an unpacked GCC release (gcc-15.2.0); only libstdc++-v3,
#             libgcc, libiberty, include, config, contrib/relpath.sh and
#             the top-level files are read
#   CPUS      the CPUs to build for (default: "m68k ppc")
#   WORK      the build directory (default: a temporary directory)
#   OUT       where the C library is, and where to put the C++ library
#             (default: lib/ in the SDK, where tools/macapp.py looks)
#
# It writes:
#   OUT/include/c++/VERSION/              the headers
#   OUT/include/c++/VERSION/m68k-apple-macos/
#   OUT/include/c++/VERSION/powerpc-apple-macos9/
#                                         the headers for each target
#   OUT/m68k/ and OUT/ppc/                libstdc++.a, libsupc++.a and
#                                         libunwind.a
#   OUT/share/licenses/libstdc++/         COPYING3 and COPYING.RUNTIME
#
# The script does not change GCC_SRC: it copies the parts that it uses and
# applies runtime/libstdcxx/gcc-VERSION.patch to the copy. Needs make and
# a POSIX shell for GCC's configure. docs/cxx.md tells more.
set -e
here=$(cd "$(dirname "$0")" && pwd)
sdk=$(dirname "$here")
: "${LLVM_SRC:?set LLVM_SRC to a clang-retro/llvm checkout}"
: "${GCC_SRC:?set GCC_SRC to an unpacked GCC release}"
CLANG=${CLANG:-clang}
clang=$(command -v "$CLANG")
bindir=$(dirname "$clang")
CPUS=${CPUS:-m68k ppc}
out=${1:-$sdk/lib}
out=$(cd "$out" && pwd)
LLVM_SRC=$(cd "$LLVM_SRC" && pwd)
GCC_SRC=$(cd "$GCC_SRC" && pwd)
[ -f "$out/include/stdio.h" ] ||
  { echo "no C library in $out: run runtime/build-libc.sh first" >&2; exit 1; }
if [ -z "$WORK" ]; then
  WORK=$(mktemp -d "${TMPDIR:-/tmp}/maccxx.XXXXXX")
  trap 'rm -rf "$WORK"' EXIT
fi
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
VERSION=$(cat "$GCC_SRC/gcc/BASE-VER")
jobs=$(nproc 2>/dev/null || echo 2)

# The copy of the GCC sources, with the changes.
src=$WORK/gcc
rm -rf "$src"
mkdir -p "$src/gcc" "$src/contrib"
find "$GCC_SRC" -maxdepth 1 -type f -exec cp {} "$src/" \;
cp -R "$GCC_SRC/libstdc++-v3" "$GCC_SRC/libgcc" "$GCC_SRC/libiberty" \
  "$GCC_SRC/include" "$GCC_SRC/config" "$src/"
cp "$GCC_SRC"/gcc/BASE-VER "$GCC_SRC"/gcc/DATESTAMP "$GCC_SRC"/gcc/DEV-PHASE \
  "$src/gcc/"
cp "$GCC_SRC/contrib/relpath.sh" "$src/contrib/"
patch -d "$src" -p1 -s <"$here/libstdcxx/gcc-$VERSION.patch"

# libstdc++ asks for the init_priority values that GCC keeps for itself
# (the standard streams are made first), and for GCC's warnings. The
# library calls the sized operator delete.
WARN="-Wno-error=init-priority-reserved -Wno-init-priority-reserved"
WARN="$WARN -Wno-unknown-warning-option -Wno-unknown-attributes"
WARN="$WARN -Wno-mismatched-tags -Wno-invalid-constexpr -fsized-deallocation"

for cpu in $CPUS; do
  case $cpu in
  m68k)
    target=m68k-apple-macos host=m68k-unknown-elf
    # Code over 32 KB: 32-bit addresses (start.S relocates them).
    flags="-mcpu=68000 -mcmodel=large" ;;
  ppc)
    target=powerpc-apple-macos9 host=powerpc-unknown-elf
    flags="" ;;
  *) echo "unknown CPU $cpu" >&2; exit 1 ;;
  esac
  # Each function and object in its own section, so that the linkers
  # leave out what a program does not use.
  flags="$flags -ffunction-sections -fdata-sections"
  mkdir -p "$out/$cpu"

  # libunwind: DWARF call frame information on 68k, the traceback tables
  # at the end of each function on PowerPC. No threads, and on 68k the
  # .eh_frame between __eh_frame_start and __eh_frame_end of codelink.py.
  uw=$WORK/unwind-$cpu
  rm -rf "$uw"
  mkdir -p "$uw"
  uwflags="--target=$target $flags -O2 -isystem $out/include
    -I$LLVM_SRC/libunwind/include -D_LIBUNWIND_HAS_NO_THREADS
    -D_LIBUNWIND_IS_NATIVE_ONLY -DNDEBUG -funwind-tables"
  [ $cpu = m68k ] && uwflags="$uwflags -D_LIBUNWIND_IS_BAREMETAL"
  for f in libunwind.cpp UnwindLevel1.c UnwindLevel1-gcc-ext.c \
           Unwind_AIXExtras.cpp UnwindRegistersSave.S UnwindRegistersRestore.S; do
    case $f in
    *.cpp) cc="$bindir/clang++ -std=c++17 -fno-exceptions -fno-rtti -nostdinc++" ;;
    *.c) cc="$bindir/clang -std=c99" ;;
    # clang's assembler does not take the XCOFF directives of these files;
    # runtime/unwind/xcoff-ppc.c has their two functions.
    *.S) [ $cpu = ppc ] && continue; cc="$bindir/clang" ;;
    esac
    $cc $uwflags -c "$LLVM_SRC/libunwind/src/$f" -o "$uw/${f%.*}.o"
  done
  if [ $cpu = ppc ]; then
    "$bindir/clang" $uwflags -Wall -Wextra -Werror \
      -c "$here/unwind/xcoff-ppc.c" -o "$uw/xcoff-ppc.o"
  fi
  rm -f "$out/$cpu/libunwind.a"
  "$bindir/llvm-ar" rcs "$out/$cpu/libunwind.a" "$uw"/*.o

  # The compiler commands. libstdc++'s configure reads the thread model
  # from "$CXX -v"; the wrapper says single, since the Mac has no threads
  # here.
  mkdir -p "$WORK/bin-$cpu"
  for tool in cc cxx; do
    case $tool in
    cc) drv=clang extra= ;;
    cxx) drv=clang++ extra="-nostdinc++ -nostdlib++" ;;
    esac
    cat >"$WORK/bin-$cpu/$tool" <<EOF
#!/bin/sh
if [ "\$*" = -v ]; then
  "$bindir/$drv" --target=$target -v 2>&1 |
    sed 's/^Thread model: .*/Thread model: single/' >&2
  exit 0
fi
exec "$bindir/$drv" --target=$target $flags -isystem "$out/include" $extra "\$@"
EOF
    chmod +x "$WORK/bin-$cpu/$tool"
  done

  # GCC has no classic Mac OS target. HOST-elf with --with-newlib selects
  # the settings that configure knows for a C library without an
  # operating system layer (picolibc comes from newlib). configure cannot
  # link test programs here, so the answers to the two link tests that
  # it makes anyway (for the stack trace library, which is off) are given.
  build=$WORK/build-$cpu
  rm -rf "$build"
  mkdir -p "$build"
  (cd "$build" &&
    "$src/libstdc++-v3/configure" \
      --build="$("$src/config.guess")" --host=$host --with-newlib \
      --prefix=/usr --with-gxx-include-dir=/include/c++/$VERSION \
      --disable-shared --enable-static --disable-multilib --disable-nls \
      --disable-libstdcxx-pch --disable-tls --disable-threads \
      --disable-libstdcxx-verbose --disable-libstdcxx-filesystem-ts \
      --disable-libstdcxx-backtrace --disable-libstdcxx-dual-abi \
      CC="$WORK/bin-$cpu/cc" CXX="$WORK/bin-$cpu/cxx" \
      AR="$bindir/llvm-ar" RANLIB="$bindir/llvm-ranlib" NM="$bindir/llvm-nm" \
      CFLAGS="-O2" CXXFLAGS="-O2 -D_GNU_SOURCE $WARN" \
      ac_cv_func_fcntl=no ac_cv_func_getexecname=no \
      >"$WORK/configure-$cpu.log" 2>&1) || {
    tail -n 40 "$build/config.log" >&2
    exit 1
  }
  make -C "$build" -j"$jobs" >"$WORK/build-$cpu.log" 2>&1 || {
    grep -n -B2 -A8 'error' "$WORK/build-$cpu.log" | head -n 80 >&2
    exit 1
  }
  rm -rf "$WORK/stage-$cpu"
  make -C "$build" install DESTDIR="$WORK/stage-$cpu" \
    >"$WORK/install-$cpu.log" 2>&1
  # The headers are the same for both targets, but for each target's own
  # (bits/c++config.h and the like), which go in a directory named after
  # the target.
  stage=$WORK/stage-$cpu/include/c++/$VERSION
  mkdir -p "$out/include/c++/$VERSION"
  cp -R "$stage/." "$out/include/c++/$VERSION/"
  rm -rf "$out/include/c++/$VERSION/$host" "$out/include/c++/$VERSION/$target"
  cp -R "$stage/$host" "$out/include/c++/$VERSION/$target"
  cp "$WORK"/stage-$cpu/usr/lib/libstdc++.a "$WORK"/stage-$cpu/usr/lib/libsupc++.a \
    "$out/$cpu/"
  echo "$cpu: $out/$cpu/libstdc++.a, libsupc++.a and libunwind.a"
done

# The licences: GPL 3 with the GCC Runtime Library Exception.
lic=$out/share/licenses/libstdc++
mkdir -p "$lic"
cp "$GCC_SRC/COPYING3" "$GCC_SRC/COPYING.RUNTIME" "$lic/"
