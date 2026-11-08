#!/usr/bin/env python3
"""Link 68k ELF objects into a classic Mac OS application.

clang --target=m68k-apple-macos makes ELF objects. This links them into
one flat image at address 0 (code, then read-only data, data, and zeroed
bss), appends a table of every absolute 32-bit pointer in it, and puts
the lot in CODE 1. CODE 0 holds a one-entry jump table that sends the
Segment Loader to __start (start.s), which adds the image's load address
to each listed pointer and calls main.

    python3 codelink.py -o Hello.bin start.o hello.o [more.o ...] [lib.a ...]
        [--name Hello] [--creator ????] [--size KB] [--map FILE]

The output is MacBinary II, for hfsutils' "hcopy -m". Put start.o first.
From archives (.a), only the members the program needs are linked.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macres import macbinary, resource_fork, size_res  # noqa: E402
import archive  # noqa: E402

SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS, SHT_RELA = 1, 2, 8, 4
SHF_ALLOC = 2
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2
STB_LOCAL = 0

R_68K_NONE, R_68K_32, R_68K_16, R_68K_8 = 0, 1, 2, 3
R_68K_PC32, R_68K_PC16, R_68K_PC8 = 4, 5, 6
R_68K_PLT32, R_68K_PLT16, R_68K_PLT8 = 13, 14, 15


class LinkError(Exception):
    pass


def align_up(x, a):
    return (x + a - 1) & ~(a - 1)


class Section:
    def __init__(self, obj, idx, name, kind, size, align, data):
        self.obj, self.idx, self.name = obj, idx, name
        self.kind, self.size, self.align, self.data = kind, size, align, data
        self.addr = None
        self.relocs = []


class ElfObject:
    def __init__(self, path, blob):
        self.path = path
        if blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 2:
            raise LinkError(f"{path}: not a 32-bit big-endian ELF file")
        (etype, machine, _, _, _, shoff, _, _, _, _, shentsize, shnum,
         shstrndx) = struct.unpack_from(">HHIIIIIHHHHHH", blob, 16)
        if etype != 1 or machine != 4:
            raise LinkError(f"{path}: not an m68k relocatable object")
        shdrs = [struct.unpack_from(">IIIIIIIIII", blob, shoff + i * shentsize)
                 for i in range(shnum)]
        shstr = shdrs[shstrndx]

        def name_at(tab, off):
            base = tab[4] + off
            return blob[base:blob.index(b"\0", base)].decode()

        self.sections = {}
        symtab = None
        for i, h in enumerate(shdrs):
            name = name_at(shstr, h[0])
            stype, flags, size, align = h[1], h[2], h[5], max(h[8], 1)
            if stype == SHT_SYMTAB:
                symtab = (h, shdrs[h[6]])
            if not flags & SHF_ALLOC or stype not in (SHT_PROGBITS,
                                                      SHT_NOBITS):
                continue
            if flags & 4:
                kind = "text"
            elif stype == SHT_NOBITS:
                kind = "bss"
            elif flags & 1:
                kind = "data"
            else:
                kind = "rodata"
            data = b"" if stype == SHT_NOBITS else blob[h[4]:h[4] + size]
            self.sections[i] = Section(self, i, name, kind, size, align, data)

        self.symbols = []
        if symtab:
            sh, strsh = symtab
            for j in range(sh[5] // 16):
                nm, value, size, info, _, shndx = struct.unpack_from(
                    ">IIIBBH", blob, sh[4] + 16 * j)
                name = name_at(strsh, nm) if nm else ""
                self.symbols.append(dict(name=name, value=value, size=size,
                                         bind=info >> 4, shndx=shndx))

        for h in shdrs:
            if h[1] != SHT_RELA or h[7] not in self.sections:
                continue
            sec = self.sections[h[7]]
            for k in range(h[5] // 12):
                off, info, addend = struct.unpack_from(">IIi", blob,
                                                       h[4] + 12 * k)
                sec.relocs.append((off, info >> 8, info & 0xFF, addend))


def defined_names(o):
    return {s["name"] for s in o.symbols
            if s["bind"] != STB_LOCAL and s["name"] and s["shndx"] != SHN_UNDEF}


def referenced_names(o):
    return {s["name"] for s in o.symbols
            if s["bind"] != STB_LOCAL and s["name"] and s["shndx"] == SHN_UNDEF}


def link(objs):
    # Lay out the image: text, rodata, data, then bss.
    order = ["text", "rodata", "data", "bss"]
    pos = 0
    placed = []
    for kind in order:
        for o in objs:
            for s in o.sections.values():
                if s.kind == kind:
                    pos = align_up(pos, s.align)
                    s.addr = pos
                    pos += s.size
                    placed.append(s)
        if kind == "data":
            pos = align_up(pos, 4)
            bss_start = pos

    # Common symbols go at the end of bss.
    globals_ = {}
    commons = {}
    for o in objs:
        for sym in o.symbols:
            if sym["bind"] == STB_LOCAL or not sym["name"]:
                continue
            if sym["shndx"] == SHN_COMMON:
                c = commons.get(sym["name"], (0, 1))
                commons[sym["name"]] = (max(c[0], sym["size"]),
                                        max(c[1], sym["value"]))
            elif sym["shndx"] != SHN_UNDEF:
                if sym["name"] in globals_:
                    raise LinkError(f"duplicate symbol {sym['name']}")
                if sym["shndx"] == SHN_ABS:
                    globals_[sym["name"]] = ("abs", sym["value"])
                else:
                    s = o.sections.get(sym["shndx"])
                    if s is None:
                        continue
                    globals_[sym["name"]] = ("rel", s.addr + sym["value"])
    for name, (size, al) in sorted(commons.items()):
        if name in globals_:
            continue
        pos = align_up(pos, al)
        globals_[name] = ("rel", pos)
        pos += size

    # The relocation table goes after bss, so __start can find it.
    pos = align_up(pos, 4)
    table_at = pos
    globals_["__reloc_table"] = ("abs", table_at)

    image = bytearray(table_at)
    for s in placed:
        if s.kind != "bss":
            image[s.addr:s.addr + s.size] = s.data

    abs_fixups = []
    for o in objs:
        for s in o.sections.values():
            for (off, symidx, rtype, addend) in s.relocs:
                if rtype == R_68K_NONE:
                    continue
                sym = o.symbols[symidx]
                if sym["bind"] != STB_LOCAL and sym["name"]:
                    if sym["name"] not in globals_:
                        raise LinkError(f"{o.path}: undefined symbol "
                                        f"{sym['name']}")
                    kind, value = globals_[sym["name"]]
                elif sym["shndx"] == SHN_ABS:
                    kind, value = "abs", sym["value"]
                else:
                    tgt = o.sections.get(sym["shndx"])
                    if tgt is None:
                        raise LinkError(f"{o.path}: relocation against "
                                        f"non-allocated section")
                    kind, value = "rel", tgt.addr + sym["value"]
                at = s.addr + off
                S = value + addend
                if rtype == R_68K_32:
                    struct.pack_into(">I", image, at, S & 0xFFFFFFFF)
                    if kind == "rel":
                        abs_fixups.append(at)
                elif rtype in (R_68K_16, R_68K_8):
                    if kind == "rel":
                        raise LinkError(f"{o.path}: 16/8-bit absolute "
                                        f"address in {s.name}+{off:#x}")
                    fmt = ">h" if rtype == R_68K_16 else ">b"
                    struct.pack_into(fmt, image, at, S)
                elif rtype in (R_68K_PC32, R_68K_PC16, R_68K_PC8,
                               R_68K_PLT32, R_68K_PLT16, R_68K_PLT8):
                    if kind == "abs":
                        raise LinkError(f"{o.path}: PC-relative reference "
                                        f"to absolute {sym['name']}")
                    d = S - at
                    if rtype in (R_68K_PC32, R_68K_PLT32):
                        struct.pack_into(">i", image, at, d)
                    elif rtype in (R_68K_PC16, R_68K_PLT16):
                        if not -0x8000 <= d < 0x8000:
                            raise LinkError(
                                f"{o.path}: {sym['name'] or s.name} is too "
                                f"far for a 16-bit PC-relative reference "
                                f"at {s.name}+{off:#x}")
                        struct.pack_into(">h", image, at, d)
                    else:
                        if not -0x80 <= d < 0x80:
                            raise LinkError(f"{o.path}: 8-bit branch out "
                                            f"of range")
                        struct.pack_into(">b", image, at, d)
                else:
                    raise LinkError(f"{o.path}: unsupported relocation "
                                    f"type {rtype} in {s.name}")

    if "__start" not in globals_ or globals_["__start"][1] != 0:
        raise LinkError("__start must come first (put start.o first)")
    if "main" not in globals_:
        raise LinkError("no main")
    abs_fixups.sort()
    image += struct.pack(">I", len(abs_fixups))
    for a in abs_fixups:
        image += struct.pack(">I", a)
    return bytes(image), globals_, bss_start


def code_resources(image):
    # CODE 0: above-A5 size, below-A5 size, jump table size and offset,
    # then one entry: offset in segment, "move.w #1,-(sp)", _LoadSeg.
    jt = struct.pack(">HHHH", 0, 0x3F3C, 1, 0xA9F0)
    code0 = struct.pack(">IIII", 32 + len(jt), 4, len(jt), 32) + jt
    # CODE 1: first jump table entry 0, one entry; then the image.
    code1 = struct.pack(">HH", 0, 1) + image
    return code0, code1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("-o", dest="output", required=True)
    ap.add_argument("--name")
    ap.add_argument("--creator", default="????")
    ap.add_argument("--size", type=int, default=1024,
                    help="memory partition in KiB")
    ap.add_argument("--map", help="write symbol addresses here")
    a = ap.parse_args()
    try:
        objs, libs = [], []
        for p in a.inputs:
            blob = open(p, "rb").read()
            if archive.is_archive(blob):
                libs.append((p, blob))
            else:
                objs.append(ElfObject(p, blob))
        objs += archive.select(objs, libs, ElfObject, defined_names,
                               referenced_names)
        image, syms, _ = link(objs)
    except (LinkError, archive.ArchiveError) as e:
        sys.exit(f"codelink: error: {e}")
    code0, code1 = code_resources(image)
    name = a.name or os.path.splitext(os.path.basename(a.output))[0]
    rsrc = resource_fork([(b"CODE", 0, code0), (b"CODE", 1, code1),
                          (b"SIZE", -1, size_res(a.size))])
    creator = a.creator.encode("mac_roman")[:4].ljust(4)
    open(a.output, "wb").write(macbinary(name, b"APPL", creator, b"", rsrc))
    if a.map:
        with open(a.map, "w") as f:
            for n, (k, v) in sorted(syms.items(), key=lambda kv: kv[1][1]):
                f.write(f"{v:08x} {k[0]} {n}\n")
    print(f"codelink: {a.output}: {len(code1)} bytes in CODE 1")


if __name__ == "__main__":
    main()
