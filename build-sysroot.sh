#!/bin/sh
# Build the classic Mac OS sysroot: one directory that has all that is
# necessary to compile and link applications for 68k and PowerPC, from
# System 6 to Mac OS 9, without this source tree.
#
#   CLANG=... LLVM_SRC=... PICOLIBC_SRC=... [GCC_SRC=...] build-sysroot.sh OUT
#
#   CLANG         clang-retro's clang (default: clang on the PATH); llvm-ar
#                 and the other LLVM tools are taken from the same directory
#   LLVM_SRC      a clang-retro/llvm checkout (compiler-rt's builtins and
#                 libunwind are read)
#   PICOLIBC_SRC  a clang-retro/picolibc checkout
#   GCC_SRC       an unpacked GCC 15.2.0 release (optional). Without it,
#                 the sysroot has no C++ library (libstdc++ and libunwind).
#   OUT           the sysroot directory. The script removes it first.
#
# OUT has:
#   include/            the Toolbox headers
#   lib/include/        picolibc's headers, and libstdc++'s in c++/
#   lib/m68k/ lib/ppc/  libmacrt.a (the builtins), libc.a, libmacos.a,
#                       libmacconsole.a, and libstdc++.a, libsupc++.a and
#                       libunwind.a
#   tools/macapp.py     the compiler and linker driver
#   tools/link/         the linkers (codelink.py, peflink.py, fatlink.py),
#                       start.S and the import index
#   tools/sim68k.py tools/simppc.py
#                       the CPU emulators that run small programs
#   examples/           the example programs
#   share/licenses/     the license of each part
#
# Every file in OUT is free software: the SDK is AGPL-3.0-only, the
# headers made from Multiversal Interfaces are MIT, picolibc is BSD,
# compiler-rt and libunwind are Apache-2.0 with the LLVM exception, and
# libstdc++ is GPL-3.0 with the GCC Runtime Library Exception. No file
# from Apple is in OUT. docs/sysroot.md tells more.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build-sysroot.sh OUT}
: "${LLVM_SRC:?set LLVM_SRC to a clang-retro/llvm checkout}"
: "${PICOLIBC_SRC:?set PICOLIBC_SRC to a clang-retro/picolibc checkout}"
CLANG=${CLANG:-clang}
export CLANG LLVM_SRC PICOLIBC_SRC

rm -rf "$out"
mkdir -p "$out/lib"
out=$(cd "$out" && pwd)

# The libraries: the builtins first, because the C library calls them,
# then the C library, then the C++ library, which is built over it.
sh "$here/runtime/build.sh" "$out/lib"
sh "$here/runtime/build-libc.sh" "$out/lib"
if [ -n "${GCC_SRC:-}" ]; then
  export GCC_SRC
  sh "$here/runtime/build-libstdcxx.sh" "$out/lib"
else
  echo "GCC_SRC is not set: the sysroot has no C++ library" >&2
fi

# The headers, the driver, the linkers and the emulators. macapp.py finds
# include/ and lib/ in the directory above its own, so it works in OUT as
# it does in this source tree.
cp -R "$here/include" "$out/include"
mkdir -p "$out/tools/link" "$out/examples"
cp "$here/tools/macapp.py" "$here/tools/sim68k.py" "$here/tools/simppc.py" \
  "$here/tools/fakegl.py" "$out/tools/"
for f in "$here"/tools/link/*.py "$here"/tools/link/*.S "$here/tools/link/imports.json"; do
  cp "$f" "$out/tools/link/"
done
cp -R "$here"/examples/* "$out/examples/"

# The licenses go with the sysroot.
lic=$out/share/licenses
mkdir -p "$lic/mac-sdk" "$lic/picolibc" "$lic/llvm"
cp "$here/LICENSE" "$lic/mac-sdk/LICENSE"
cp "$here/defs/COPYING.multiversal" "$lic/mac-sdk/COPYING.multiversal"
cp "$PICOLIBC_SRC/COPYING.picolibc" "$lic/picolibc/"
# A sparse checkout of LLVM can lack some of these files; each part of
# LLVM has the same license, so one of them is enough.
for p in compiler-rt libunwind llvm; do
  if [ -f "$LLVM_SRC/$p/LICENSE.TXT" ]; then
    cp "$LLVM_SRC/$p/LICENSE.TXT" "$lic/llvm/$p-LICENSE.TXT"
  fi
done
ls "$lic/llvm/"*-LICENSE.TXT >/dev/null 2>&1 ||
  { echo "no LICENSE.TXT of LLVM in $LLVM_SRC" >&2; exit 1; }
# build-libstdcxx.sh puts the licenses of libstdc++ in lib/share/licenses.
if [ -d "$out/lib/share/licenses" ]; then
  cp -R "$out/lib/share/licenses/"* "$lic/"
  rm -rf "$out/lib/share"
fi
cp "$here/docs/sysroot.md" "$out/README.md"
echo "built $out"
