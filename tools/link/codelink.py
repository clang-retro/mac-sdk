#!/usr/bin/env python3
"""Link 68k ELF objects into a classic Mac OS application.

clang --target=m68k-apple-macos makes ELF objects. This links them into
one flat image at address 0 (code, then read-only data, data, and zeroed
bss), appends a table of every absolute 32-bit pointer in it, and puts
the lot in CODE 1. CODE 0 holds a one-entry jump table that sends the
Segment Loader to __start (start.S), which adds the image's load address
to each listed pointer and calls main.

    python3 codelink.py -o Hello.bin start.o hello.o [more.o ...] [lib.a ...]
        [--name Hello] [--creator ????] [--size KB] [--map FILE]

The output is MacBinary II, for hfsutils' "hcopy -m". Put start.o first.
From archives (.a), only the members the program needs are linked.

For C++, the linker also:
- keeps one copy of each COMDAT group (inline functions, templates, type
  information) and lets a strong definition replace weak ones;
- puts every .eh_frame together between __eh_frame_start and
  __eh_frame_end, for the unwinder (libunwind), and removes the frame
  descriptions of the functions it removed;
- removes the sections that nothing reaches from __start, the
  constructor and destructor tables or a kept frame description
  (--no-gc-sections keeps them all). Compile with -ffunction-sections
  and -fdata-sections to make this remove most.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macres import macbinary, resource_fork, size_res  # noqa: E402
import archive  # noqa: E402

SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS, SHT_RELA = 1, 2, 8, 4
SHT_INIT_ARRAY, SHT_FINI_ARRAY, SHT_PREINIT_ARRAY = 14, 15, 16
SHT_GROUP = 17
GRP_COMDAT = 1
SHF_ALLOC = 2
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2
STB_LOCAL, STB_GLOBAL, STB_WEAK = 0, 1, 2

R_68K_NONE, R_68K_32, R_68K_16, R_68K_8 = 0, 1, 2, 3
R_68K_PC32, R_68K_PC16, R_68K_PC8 = 4, 5, 6
R_68K_PLT32, R_68K_PLT16, R_68K_PLT8 = 13, 14, 15


class LinkError(Exception):
    """An error in the input objects, which the linker reports and stops."""
    pass


def align_up(x, a):
    """Round x up to a multiple of a (a power of two)."""
    return (x + a - 1) & ~(a - 1)


class Section:
    """One allocated section of an object, as the linker places it."""

    def __init__(self, obj, idx, name, kind, size, align, data):
        """Keep the section's place in its object, its kind, size, alignment
        and contents."""
        self.obj, self.idx, self.name = obj, idx, name
        self.kind, self.size, self.align, self.data = kind, size, align, data
        self.addr = None
        self.relocs = []
        self.live = False
        self.fdes = []            # .eh_frame: its records (EhRecord)


class EhRecord:
    """One CIE or FDE of an .eh_frame section.

    off and size give the bytes in the section; cie is the CIE of an FDE;
    relocs are the section's relocations inside the record.
    """

    def __init__(self, off, size, cie):
        """Keep the record's offset and size in .eh_frame, and whether it is a
        CIE."""
        self.off, self.size, self.cie = off, size, cie
        self.relocs = []
        self.live = False


class ElfObject:
    """One ELF relocatable object: its sections, groups, symbols and
    relocations."""
    def __init__(self, path, blob):
        """Read the sections, COMDAT groups, symbols and relocations of an ELF
        object."""
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
            """Read the name at offset off of the string table tab."""
            base = tab[4] + off
            return blob[base:blob.index(b"\0", base)].decode()

        self.sections = {}
        self.groups = []          # (signature symbol index, [section indexes])
        symtab = None
        for i, h in enumerate(shdrs):
            name = name_at(shstr, h[0])
            stype, flags, size, align = h[1], h[2], h[5], max(h[8], 1)
            if stype == SHT_SYMTAB:
                symtab = (h, shdrs[h[6]])
            if stype == SHT_GROUP:
                words = struct.unpack_from(f">{size // 4}I", blob, h[4])
                if words and words[0] & GRP_COMDAT:
                    self.groups.append((h[7], list(words[1:])))
                continue
            if not flags & SHF_ALLOC or stype not in (
                    SHT_PROGBITS, SHT_NOBITS, SHT_INIT_ARRAY,
                    SHT_FINI_ARRAY, SHT_PREINIT_ARRAY):
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
        for sec in self.sections.values():
            sec.relocs.sort()
            if sec.name == ".eh_frame":
                split_eh_frame(self, sec)

    def group_signature(self, symidx):
        """Return the name of a group's signature symbol."""
        return self.symbols[symidx]["name"]


def split_eh_frame(o, sec):
    """Divide an .eh_frame section into its CIE and FDE records."""
    data, off, cies = sec.data, 0, {}
    while off + 4 <= len(data):
        length = struct.unpack_from(">I", data, off)[0]
        if length == 0:
            break                       # terminator
        if length == 0xFFFFFFFF:
            raise LinkError(f"{o.path}: 64-bit .eh_frame record")
        cie_id = struct.unpack_from(">I", data, off + 4)[0]
        cie = None
        if cie_id:
            cie = cies.get(off + 4 - cie_id)
            if cie is None:
                raise LinkError(f"{o.path}: .eh_frame FDE at {off:#x} has "
                                f"no CIE")
        rec = EhRecord(off, 4 + length, cie)
        if not cie_id:
            cies[off] = rec
        rec.relocs = [r for r in sec.relocs if off <= r[0] < off + 4 + length]
        sec.fdes.append(rec)
        off += 4 + length


def defined_names(o):
    """Give the global names that object o defines."""
    return {s["name"] for s in o.symbols
            if s["bind"] != STB_LOCAL and s["name"] and s["shndx"] != SHN_UNDEF}


def referenced_names(o):
    """Give the global names that object o uses and does not define."""
    return {s["name"] for s in o.symbols
            if s["bind"] != STB_LOCAL and s["name"] and s["shndx"] == SHN_UNDEF}


def drop_duplicate_groups(objs):
    """Keep the first copy of each COMDAT group and remove the others."""
    seen = set()
    for o in objs:
        for symidx, members in o.groups:
            sig = o.group_signature(symidx)
            if sig not in seen:
                seen.add(sig)
                continue
            for i in members:
                o.sections.pop(i, None)


def target_section(o, symidx, globals_):
    """Return the section a relocation's symbol is in, or None."""
    sym = o.symbols[symidx]
    if sym["bind"] != STB_LOCAL and sym["name"]:
        d = globals_.get(sym["name"])
        return d[2] if d else None
    return o.sections.get(sym["shndx"])


def collect_garbage(objs, globals_, gc):
    """Mark the sections the program reaches; with gc, drop the others.

    The roots are __start and the constructor and destructor tables. An
    FDE is kept when its function is, and then keeps its LSDA and its
    CIE (with the personality function).
    """
    work = []

    def mark(sec):
        """Mark section sec live and queue it for the scan of its relocations."""
        if sec is not None and not sec.live:
            sec.live = True
            work.append(sec)

    for o in objs:
        for s in o.sections.values():
            if not gc or s.name.startswith((".init_array", ".fini_array",
                                            ".preinit_array")) or \
                    s.name in (".text.__start",):
                mark(s)
    if "__start" in globals_:
        mark(globals_["__start"][2])
    while True:
        while work:
            sec = work.pop()
            if sec.name == ".eh_frame":
                continue                # its records are marked below
            for (_, symidx, rtype, _) in sec.relocs:
                if rtype != R_68K_NONE:
                    mark(target_section(sec.obj, symidx, globals_))
        # Keep each FDE whose function is kept, and what it refers to.
        more = False
        for o in objs:
            for sec in o.sections.values():
                for rec in sec.fdes:
                    if rec.live or rec.cie is None or not rec.relocs:
                        continue
                    fn = target_section(o, rec.relocs[0][1], globals_)
                    if fn is None or not fn.live:
                        continue
                    for r in (rec, rec.cie):
                        if r.live:
                            continue
                        r.live = True
                        more = True
                        for (_, symidx, _, _) in r.relocs:
                            t = target_section(o, symidx, globals_)
                            if t is not sec:
                                mark(t)
                    sec.live = True
        if not work and not more:
            break
    for o in objs:
        for i, s in list(o.sections.items()):
            if not s.live:
                del o.sections[i]
            elif s.fdes:
                rebuild_eh_frame(s)


def rebuild_eh_frame(sec):
    """Keep only the live records of an .eh_frame section.

    The CIE pointer of each FDE is a distance back to its CIE, so it is
    computed again for the new layout, and the relocations move with
    their records.
    """
    out, relocs, new_off = bytearray(), [], {}
    for rec in sec.fdes:
        if not rec.live:
            continue
        new_off[rec] = len(out)
        body = bytearray(sec.data[rec.off:rec.off + rec.size])
        if rec.cie is not None:
            struct.pack_into(">I", body, 4, len(out) + 4 - new_off[rec.cie])
        for (off, symidx, rtype, addend) in rec.relocs:
            relocs.append((off - rec.off + len(out), symidx, rtype, addend))
        out += body
    sec.data, sec.size, sec.relocs = bytes(out), len(out), relocs


def symbol_table(objs):
    """Choose the definition of each global name.

    Returns name -> (bind, object, symbol, section or None). A strong
    definition replaces a weak one; two strong ones are an error. A
    symbol in a section that was removed (a duplicate COMDAT group) is
    left out, since the kept copy defines it too.
    """
    table = {}
    for o in objs:
        for sym in o.symbols:
            if sym["bind"] == STB_LOCAL or not sym["name"] or \
                    sym["shndx"] in (SHN_UNDEF, SHN_COMMON):
                continue
            sec = None
            if sym["shndx"] != SHN_ABS:
                sec = o.sections.get(sym["shndx"])
                if sec is None:
                    continue
            old = table.get(sym["name"])
            if old is not None:
                if sym["bind"] == STB_WEAK or old[0] == STB_GLOBAL and \
                        sym["bind"] == STB_WEAK:
                    continue
                if old[0] == STB_GLOBAL:
                    raise LinkError(f"duplicate symbol {sym['name']}")
            table[sym["name"]] = (sym["bind"], o, sym, sec)
    return table


def link(objs, gc=True):
    """Place the live sections of objs in one image, apply the relocations, and
    give the image with its relocation table."""
    drop_duplicate_groups(objs)
    table = symbol_table(objs)
    collect_garbage(objs, {n: (None, None, d[3]) for n, d in table.items()},
                    gc)

    # Lay out the image: text, rodata, data, then bss.
    # The constructor and destructor tables (.preinit_array, .init_array,
    # .fini_array) go first in data, each in one piece, with the
    # __*_array_start and __*_array_end symbols that the C library's
    # __libc_init_array and __libc_fini_array walk. Every .eh_frame goes
    # last in rodata, in one piece, between __eh_frame_start and
    # __eh_frame_end, followed by a zero word.
    order = ["text", "rodata", "data", "bss"]
    pos = 0
    placed = []
    tables = {}

    def table_of(s):
        """Give the constructor or destructor table that section s is part of,
        or None."""
        for t in ("preinit_array", "init_array", "fini_array"):
            if s.name == "." + t or s.name.startswith("." + t + "."):
                return t
        return None

    def priority(s):
        """Give the priority of a constructor or destructor table section
        (65536 when it has none)."""
        tail = s.name.rsplit(".", 1)[-1]
        return int(tail) if tail.isdigit() else 65536

    for kind in order:
        if kind == "data":
            for t in ("preinit_array", "init_array", "fini_array"):
                secs = [s for o in objs for s in o.sections.values()
                        if s.kind == "data" and table_of(s) == t]
                secs.sort(key=priority)    # stable: link order otherwise
                pos = align_up(pos, 4)
                start = pos
                for s in secs:
                    s.addr = pos
                    pos += s.size
                    placed.append(s)
                tables[t] = (start, pos)
        for o in objs:
            for s in o.sections.values():
                if s.kind == kind and s.addr is None and \
                        s.name != ".eh_frame":
                    pos = align_up(pos, s.align)
                    s.addr = pos
                    pos += s.size
                    placed.append(s)
        if kind == "rodata":
            pos = align_up(pos, 4)
            tables["eh_frame"] = [pos, None]
            for o in objs:
                for s in o.sections.values():
                    if s.name == ".eh_frame" and s.addr is None:
                        pos = align_up(pos, s.align)
                        s.addr = pos
                        pos += s.size
                        placed.append(s)
            tables["eh_frame"][1] = pos
            pos += 4                       # the terminating zero word
        if kind == "data":
            pos = align_up(pos, 4)
            bss_start = pos

    # Common symbols go at the end of bss.
    globals_ = {}
    for t, (start, end) in tables.items():
        globals_[f"__{t}_start"] = ("rel", start)
        globals_[f"__{t}_end"] = ("rel", end)
    globals_["__bothinit_array_start"] = ("rel", tables["preinit_array"][0])
    globals_["__bothinit_array_end"] = ("rel", tables["init_array"][1])
    # No .eh_frame_hdr: libunwind then searches .eh_frame itself.
    globals_["__eh_frame_hdr_start"] = ("abs", 0)
    globals_["__eh_frame_hdr_end"] = ("abs", 0)
    for name, (_, o, sym, sec) in table.items():
        if name in globals_:
            raise LinkError(f"duplicate symbol {name}")
        if sec is None:
            globals_[name] = ("abs", sym["value"])
        elif sec.addr is not None:
            globals_[name] = ("rel", sec.addr + sym["value"])
    commons = {}
    for o in objs:
        for sym in o.symbols:
            if sym["bind"] != STB_LOCAL and sym["name"] and \
                    sym["shndx"] == SHN_COMMON:
                c = commons.get(sym["name"], (0, 1))
                commons[sym["name"]] = (max(c[0], sym["size"]),
                                        max(c[1], sym["value"]))
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
                    if sym["name"] in globals_:
                        kind, value = globals_[sym["name"]]
                    elif sym["bind"] == STB_WEAK:
                        kind, value = "abs", 0     # undefined weak
                    else:
                        raise LinkError(f"{o.path}: undefined symbol "
                                        f"{sym['name']}")
                elif sym["shndx"] == SHN_ABS:
                    kind, value = "abs", sym["value"]
                else:
                    tgt = o.sections.get(sym["shndx"])
                    if tgt is None:
                        raise LinkError(f"{o.path}: relocation in {s.name} "
                                        f"against a removed or "
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
    """Make CODE 0 (the jump table) and CODE 1 (the image) from the linked
    image."""
    # CODE 0: above-A5 size, below-A5 size, jump table size and offset,
    # then one entry: offset in segment, "move.w #1,-(sp)", _LoadSeg.
    jt = struct.pack(">HHHH", 0, 0x3F3C, 1, 0xA9F0)
    code0 = struct.pack(">IIII", 32 + len(jt), 4, len(jt), 32) + jt
    # CODE 1: first jump table entry 0, one entry; then the image.
    code1 = struct.pack(">HH", 0, 1) + image
    return code0, code1


def main():
    """Read the command line, link the objects and write the application."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("-o", dest="output", required=True)
    ap.add_argument("--name")
    ap.add_argument("--creator", default="????")
    ap.add_argument("--size", type=int, default=1024,
                    help="memory partition in KiB")
    ap.add_argument("--map", help="write symbol addresses here")
    ap.add_argument("--no-gc-sections", dest="gc", action="store_false",
                    help="keep the sections that nothing refers to")
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
        image, syms, _ = link(objs, a.gc)
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
