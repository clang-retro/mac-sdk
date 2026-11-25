#!/usr/bin/env python3
"""Compile C sources and link them into a classic Mac OS application.

    python3 tools/macapp.py -o Hello.bin [--cpu fat|68k|ppc] [--name Hello]
        [--creator ????] [--size KB] [--carbon] [--no-runtime] [--no-libc]
        [--lib DIR] [--large] [--console file|window]
        [-- clang flags] hello.c ...

C++ sources (.cpp, .cc, .cxx) compile against GCC's C++ library when
runtime/build-libstdcxx.sh has made it (lib/include/c++ and
lib/*/libstdc++.a), and the program links with it and with libunwind.a,
which throws exceptions. Every source is compiled with -ffunction-sections
and -fdata-sections, so that the linkers leave out what is not used.

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
__crt_main, which calls exit(main(argc, argv)). The console (stdin,
stdout and stderr) is a pair of files next to the program, or with
--console window a window (libmacconsole.a). The output is MacBinary
II, for hfsutils' "hcopy -m".

68k code reaches its data PC-relative, within 32 KB, unless compiled
with -mcmodel=large, which uses 32-bit addresses (start.S relocates
them at launch). When the 68k link fails because a reference is out of
that reach, macapp.py compiles the sources again with -mcmodel=large;
--large does so from the start. The libraries are built that way.
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
CXX_SUFFIXES = (".cpp", ".cc", ".cxx", ".c++", ".C")


def is_cxx(src):
    """Tell if a source file is C++ (by its suffix)."""
    return src.endswith(CXX_SUFFIXES)


def cxx_include(lib, target):
    """Return the include flags of GCC's C++ library for a target, or None.

    The headers are in lib/include/c++/VERSION, with the target's own in
    a directory named after the target; the newest version is used.
    """
    base = os.path.join(lib, "include", "c++")
    if not os.path.isdir(base):
        return None
    versions = sorted((v for v in os.listdir(base)
                       if os.path.isdir(os.path.join(base, v, target))),
                      key=lambda v: [int(x) if x.isdigit() else 0
                                     for x in v.split(".")])
    if not versions:
        return None
    top = os.path.join(base, versions[-1])
    return ["-nostdinc++", "-isystem", os.path.join(top, target),
            "-isystem", top]


def run(cmd):
    """Run cmd and stop with its exit status if it fails."""
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(r.returncode)


def link_68k(cmd):
    """Run codelink.py; False when a reference was out of 16-bit reach."""
    r = subprocess.run(cmd, stderr=subprocess.PIPE, text=True)
    if r.returncode and "16-bit PC-relative" in r.stderr:
        return False
    sys.stderr.write(r.stderr)
    if r.returncode:
        sys.exit(r.returncode)
    return True


def build(cpu, a, clang, tmp, large=False):
    """Compile the sources for one CPU and link them; give the linked file."""
    target = TARGETS[cpu]
    libdir = os.path.join(a.lib, {"68k": "m68k"}.get(cpu, cpu))
    # The C library, when runtime/build-libc.sh has made it.
    libc = a.libc and os.path.exists(os.path.join(libdir, "libc.a"))
    inc = ["-I", os.path.join(SDK, "include")]
    if libc:
        inc = ["-isystem", os.path.join(a.lib, "include")] + inc
    # C++: the C++ library's headers come before the C library's, since
    # they include them with #include_next.
    cxx = any(is_cxx(src) for src in a.sources)
    cxxinc = cxx_include(a.lib, target) if cxx and libc else None
    if cxx and libc and cxxinc is None:
        sys.exit("macapp: no C++ library in " + a.lib + "; run "
                 "runtime/build-libstdcxx.sh")
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
        model = ["-mcmodel=large"] if large else []
        lang = (cxxinc or []) if is_cxx(src) else []
        run(clang + [f"--target={target}", "-O2", "-ffunction-sections",
                     "-fdata-sections"] + model + lang + inc + a.cflags +
            ["-c", src, "-o", obj])
        objs.append(obj)
    libs = ["libmacos.a", "libc.a"] if libc else []
    if cxxinc:
        libs = ["libstdc++.a", "libunwind.a"] + libs
    if libc and a.console == "window":
        libs.insert(0, "libmacconsole.a")
    if a.runtime:
        libs.append("libmacrt.a")
    objs += [os.path.join(libdir, n) for n in libs
             if os.path.exists(os.path.join(libdir, n))]
    out = os.path.join(tmp, f"{cpu}.bin") if a.cpu == "fat" else a.output
    args = ["-o", out, "--name", a.name, "--size", str(a.size)]
    if cpu == "68k":
        if not link_68k([sys.executable, os.path.join(LINK, "codelink.py"),
                         "--creator", a.creator] + args + objs):
            if large:
                # Large already: what is out of reach is not ours to fix.
                run([sys.executable, os.path.join(LINK, "codelink.py"),
                     "--creator", a.creator] + args + objs)
            print("macapp: 68k code is out of 16-bit reach; compiling "
                  "again with -mcmodel=large", file=sys.stderr)
            return build(cpu, a, clang, tmp, large=True)
    else:
        run([sys.executable, os.path.join(LINK, "peflink.py"),
             "--creator", a.creator] + (["--carbon"] if a.carbon else []) +
            (["--entry", "__crt_main"] if libc else []) + args + objs)
    return out


def main():
    """Read the command line and build the application."""
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
    ap.add_argument("--console", choices=["file", "window"], default="file",
                    help="the C library's console: files next to the "
                    "program, or a window")
    ap.add_argument("--large", action="store_true",
                    help="68k: compile with -mcmodel=large from the start")
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
            build(a.cpu, a, clang, tmp, a.large)
            return
        m68k = build("68k", a, clang, tmp, a.large)
        ppc = build("ppc", a, clang, tmp)
        run([sys.executable, os.path.join(LINK, "fatlink.py"), "-o",
             a.output, "--name", a.name, m68k, ppc])


if __name__ == "__main__":
    main()
