#!/usr/bin/env python3
"""Merge a 68k application and a PowerPC application into one fat one.

    python3 fatlink.py -o Hello.bin Hello68.bin HelloPPC.bin [--name Hello]

The inputs are MacBinary files from codelink.py and peflink.py. A
fat application keeps the PowerPC code in the data fork, named by the
'cfrg' resource, and the 68k code in CODE resources. A Power Mac's Code
Fragment Manager finds the 'cfrg' and runs the PowerPC code; a 68k Mac
ignores the data fork and runs CODE as usual.
"""
import argparse
import struct
import sys

from macres import macbinary, read_macbinary, read_resource_fork, \
    resource_fork


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("m68k")
    ap.add_argument("ppc")
    ap.add_argument("-o", dest="output", required=True)
    ap.add_argument("--name")
    a = ap.parse_args()
    _, _, creator, d68, r68 = read_macbinary(open(a.m68k, "rb").read())
    name, ftype, _, dppc, rppc = read_macbinary(open(a.ppc, "rb").read())
    res68, resppc = read_resource_fork(r68), read_resource_fork(rppc)
    if not any(t == b"CODE" for t, _, _ in res68):
        sys.exit(f"fatlink: {a.m68k} has no CODE resources")
    if not any(t == b"cfrg" for t, _, _ in resppc) or not dppc:
        sys.exit(f"fatlink: {a.ppc} is not a PowerPC application")
    if d68:
        sys.exit(f"fatlink: {a.m68k} has a data fork; it would be lost")
    merged = {(t, i): b for t, i, b in res68}
    for t, i, b in resppc:
        if t == b"SIZE" and (t, i) in merged:
            # Keep the larger memory partition of the two.
            if struct.unpack_from(">I", b, 2)[0] < \
                    struct.unpack_from(">I", merged[(t, i)], 2)[0]:
                continue
        merged[(t, i)] = b
    rsrc = resource_fork([(t, i, b) for (t, i), b in merged.items()])
    open(a.output, "wb").write(
        macbinary(a.name or name, ftype, creator, dppc, rsrc))
    print(f"fatlink: {a.output}: {len(dppc)} bytes of PowerPC code, "
          f"{sum(len(b) for (t, _), b in merged.items() if t == b'CODE')}"
          f" bytes of 68k CODE")


if __name__ == "__main__":
    main()
