#!/bin/sh
# build.sh - build Quesa, the open-source QuickDraw 3D, for the classic
# Mac OS on PowerPC.
#
# Usage: extras/quesa/build.sh QUESA_SRC LIB
#
# QUESA_SRC is a checkout of https://github.com/jwwalker/Quesa at commit
# 883c9a0d218bd18c17f2f62024c0e57f2d5d4a07 (July 2007), the last commit
# that builds for the classic Mac OS. LIB is the directory where
# runtime/build.sh, runtime/build-libc.sh and runtime/build-libstdcxx.sh
# put the libraries. CLANG names the compiler (default: clang).
#
# The script copies the sources to a temporary directory, applies
# quesa.patch, compiles the files in files.txt as C++ (as CodeWarrior did
# for the classic target), and writes:
#   LIB/ppc/libquesa.a      the library
#   LIB/include/Quesa/      Quesa's headers, and QD3D*.h names for them
# See docs/quesa.md.
set -e
if [ $# -ne 2 ]; then
  echo "usage: $0 QUESA_SRC LIB" >&2
  exit 2
fi
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=$(cd "$HERE/../.." && pwd)
SRC=$(cd "$1" && pwd)
LIB=$(cd "$2" && pwd)
CLANG=${CLANG:-clang}
COMMIT=883c9a0d218bd18c17f2f62024c0e57f2d5d4a07

if [ -d "$SRC/.git" ] || [ -f "$SRC/.git" ]; then
  have=$(git -C "$SRC" rev-parse HEAD)
  if [ "$have" != "$COMMIT" ]; then
    echo "build.sh: $SRC is at $have, not $COMMIT" >&2
    exit 1
  fi
fi
CXXTOP=$(ls -d "$LIB"/include/c++/*/powerpc-apple-macos9 2>/dev/null | sort | tail -1)
if [ -z "$CXXTOP" ]; then
  echo "build.sh: no C++ library in $LIB; run runtime/build-libstdcxx.sh" >&2
  exit 1
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/quesa.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/src" "$WORK/obj"
cp -R "$SRC/Development" "$SRC/SDK" "$WORK/src/"
(cd "$WORK/src" && patch -s -p1 < "$HERE/quesa.patch")

# Every source directory is on the include path, as in the CodeWarrior
# project, except those of the other platforms.
INC=""
for d in $(cd "$WORK/src" && find Development/Source -type d |
           grep -v 'Platform/\(Windows\|Unix\|Cocoa\)\|Resources\|Gestalt' |
           sort); do
  INC="$INC -I$WORK/src/$d"
done
INC="$INC -I$WORK/src/SDK/Includes/Quesa -I$HERE/compat"

FLAGS="--target=powerpc-apple-macos9 -Os -ffunction-sections -fdata-sections
  -x c++ -std=gnu++98 -nostdinc++ -isystem $CXXTOP -isystem $(dirname "$CXXTOP")
  -isystem $LIB/include -I$SDK/include -include $HERE/compat/quesa-classic.h
  $INC -w"

# Each line of files.txt is compiled into WORK/obj, in parallel. The
# object's name is the source's path with "_" for "/".
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
export CLANG FLAGS WORK
grep -v '^#' "$HERE/files.txt" | grep . |
  xargs -P "$JOBS" -n 1 sh -c \
    '$CLANG $FLAGS -c "$WORK/src/$1" -o "$WORK/obj/$(echo "$1" | tr / _).o"' sh

n=$(grep -v '^#' "$HERE/files.txt" | grep -c .)
got=$(ls "$WORK/obj" | grep -c '\.o$')
if [ "$got" -ne "$n" ]; then
  echo "build.sh: $got of $n files compiled" >&2
  exit 1
fi

AR=${AR:-$(dirname "$(command -v "$CLANG")")/llvm-ar}
[ -x "$AR" ] || AR=llvm-ar
mkdir -p "$LIB/ppc" "$LIB/include/Quesa"
rm -f "$LIB/ppc/libquesa.a"
"$AR" rcs "$LIB/ppc/libquesa.a" "$WORK"/obj/*.o

# The headers, and the names that QuickDraw 3D programs include.
cp "$WORK/src/SDK/Includes/Quesa/"*.h "$LIB/include/Quesa/"
for h in "$LIB/include/Quesa/"Quesa*.h; do
  b=$(basename "$h" .h)
  q="QD3D${b#Quesa}"
  printf '/* %s.h - the QuickDraw 3D name of %s.h. */\n#include "%s.h"\n' \
    "$q" "$b" "$b" > "$LIB/include/Quesa/$q.h"
done
echo "quesa: $n files, $LIB/ppc/libquesa.a"
