#!/usr/bin/env python3
"""Compile C sources and link them into a classic Mac OS application.

    python3 tools/macapp.py -o Hello.bin [--cpu fat|68k|ppc] [--name Hello]
        [--creator ????] [--size KB] [--carbon] [--no-runtime] [--no-libc]
        [--lib DIR]
        [-- clang flags] hello.c ...

It compiles each source with clang (the CLANG environment variable, or
clang on the PATH) for m68k-apple-macos and powerpc-apple-macos9 with
this SDK's headers, links the 68k objects with tools/link/start.S by
codelink.py and the PowerPC ones by peflink.py, and with --cpu fat (the
default) merges the two with fatlink.py. Objects (.o) are linked as
they are, for the CPU named by --cpu (not fat), and so are archives (.a),
from which only the members needed are linked. The runtime library that
runtime/build.sh makes (lib/m68k/libmacrt.a and lib/ppc/libmacrt.a) is
linked last when it is there. When runtime/build-libc.sh has made the C
library (lib/include, lib/*/libc.a and libmacos.a), sources compile
against its headers and link with it, and the program starts at its
__crt_main, which calls exit(main(argc, argv)). The output is MacBinary
II, for hfsutils' "hcopy -m".
"""
import argparse
import os
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
LINK = os.path.join(HERE, "link")
TARGETS = {"68k": "m68k-apple-macos", "ppc": "powerpc-apple-macos9"}


def run(cmd):
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(r.returncode)


def build(cpu, a, clang, tmp):
    target = TARGETS[cpu]
    libdir = os.path.join(a.lib, {"68k": "m68k"}.get(cpu, cpu))
    # The C library, when runtime/build-libc.sh has made it.
    libc = a.libc and os.path.exists(os.path.join(libdir, "libc.a"))
    inc = ["-I", os.path.join(SDK, "include")]
    if libc:
        inc = ["-isystem", os.path.join(a.lib, "include")] + inc
    objs = []
    if cpu == "68k":
        start = os.path.join(tmp, "start.o")
        run(clang + [f"--target={target}", "-c"] +
            (["-DMAC_LIBC"] if libc else []) +
            [os.path.join(LINK, "start.S"), "-o", start])
        objs.append(start)
    for i, src in enumerate(a.sources):
        if src.endswith((".o", ".a")):
            objs.append(src)
            continue
        obj = os.path.join(tmp, f"{cpu}-{i}-{os.path.basename(src)}.o")
        run(clang + [f"--target={target}", "-O2"] + inc + a.cflags +
            ["-c", src, "-o", obj])
        objs.append(obj)
    libs = ["libmacos.a", "libc.a"] if libc else []
    if a.runtime:
        libs.append("libmacrt.a")
    objs += [os.path.join(libdir, n) for n in libs
             if os.path.exists(os.path.join(libdir, n))]
    out = os.path.join(tmp, f"{cpu}.bin") if a.cpu == "fat" else a.output
    args = ["-o", out, "--name", a.name, "--size", str(a.size)]
    if cpu == "68k":
        run([sys.executable, os.path.join(LINK, "codelink.py"),
             "--creator", a.creator] + args + objs)
    else:
        run([sys.executable, os.path.join(LINK, "peflink.py"),
             "--creator", a.creator] + (["--carbon"] if a.carbon else []) +
            (["--entry", "__crt_main"] if libc else []) + args + objs)
    return out


def main():
    argv = sys.argv[1:]
    cflags = []
    if "--" in argv:
        i = argv.index("--")
        argv, rest = argv[:i], argv[i + 1:]
        # Flags start with "-"; the rest are sources.
        cflags = [x for x in rest if x.startswith("-")]
        argv += [x for x in rest if not x.startswith("-")]
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("sources", nargs="+")
    ap.add_argument("-o", dest="output", required=True)
    ap.add_argument("--cpu", choices=["fat", "68k", "ppc"], default="fat")
    ap.add_argument("--name")
    ap.add_argument("--creator", default="????")
    ap.add_argument("--size", type=int, default=1024,
                    help="memory partition in KiB")
    ap.add_argument("--lib", default=os.path.join(SDK, "lib"),
                    help="where runtime/build.sh put the runtime library")
    ap.add_argument("--no-runtime", dest="runtime", action="store_false",
                    help="do not link the runtime library")
    ap.add_argument("--no-libc", dest="libc", action="store_false",
                    help="do not use the C library")
    ap.add_argument("--carbon", action="store_true",
                    help="PowerPC: import from CarbonLib first")
    a = ap.parse_args(argv)
    a.cflags = cflags
    a.name = a.name or os.path.splitext(os.path.basename(a.output))[0]
    if a.cpu == "fat" and any(s.endswith((".o", ".a")) for s in a.sources):
        sys.exit("macapp: objects are for one CPU; use --cpu 68k or ppc")
    clang = shlex.split(os.environ.get("CLANG", "clang"))
    with tempfile.TemporaryDirectory() as tmp:
        if a.cpu != "fat":
            build(a.cpu, a, clang, tmp)
            return
        m68k = build("68k", a, clang, tmp)
        ppc = build("ppc", a, clang, tmp)
        run([sys.executable, os.path.join(LINK, "fatlink.py"), "-o",
             a.output, "--name", a.name, m68k, ppc])


if __name__ == "__main__":
    main()
