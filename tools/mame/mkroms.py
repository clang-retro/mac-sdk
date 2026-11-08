#!/usr/bin/env python3
"""Build MAME ROM sets for Mac models from ROM dumps you own.

    mkroms.py OUTDIR ARCHIVE.zip [ARCHIVE.zip ...] -- MACHINE [MACHINE ...]

MAME wants the ROMs split the way they sat on the board (even and odd
bytes, or four byte lanes) and named after the chips. This reads every
file in the archives, tries those splits, and matches them against the
CRCs that `mame -listroms MACHINE` prints. It writes OUTDIR/MACHINE.zip
and says which chips it could not find.
"""
import os
import re
import subprocess
import sys
import zipfile
import zlib


def main():
    argv = sys.argv[1:]
    if "--" not in argv or len(argv) < 4:
        sys.exit(__doc__)
    sep = argv.index("--")
    outdir, archives, machines = argv[0], argv[1:sep], argv[sep + 1:]
    pool = {}
    for a in archives:
        z = zipfile.ZipFile(a)
        for n in z.namelist():
            if n.endswith("/"):
                continue
            d = z.read(n)
            cands = [d]
            if len(d) >= 0x10000:
                cands += [d[0::2], d[1::2]] + [d[i::4] for i in range(4)]
            for c in cands:
                pool.setdefault(zlib.crc32(c) & 0xFFFFFFFF, c)
    os.makedirs(outdir, exist_ok=True)
    for m in machines:
        out = subprocess.run(["mame", "-listroms", m], capture_output=True, text=True).stdout
        got, miss = {}, []
        for name, crc in re.findall(r"^(\S+)\s+\d+\s+CRC\(([0-9a-f]{8})\)", out, re.M):
            if int(crc, 16) in pool:
                got[name] = pool[int(crc, 16)]
            else:
                miss.append(name)
        with zipfile.ZipFile(os.path.join(outdir, m + ".zip"), "w") as o:
            for n, d in got.items():
                o.writestr(n, d)
        print(m, "found", len(got), "missing", " ".join(miss) or "nothing")


if __name__ == "__main__":
    main()
