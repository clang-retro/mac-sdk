#!/usr/bin/env python3
"""Link PowerPC XCOFF objects into a classic Mac OS PEF application.

clang --target=powerpc-apple-macos9 makes AIX-style XCOFF objects: code
calls other functions with "bl .Name" followed by a nop, and data is
reached through the TOC (r2). This tool lays the objects out as one PEF
code section and one PEF data section, adds a glue stub for every
function imported from a shared library (InterfaceLib and friends),
writes the PEF loader section, and wraps the result as a Mac application
with a 'cfrg' resource (and a 'SIZE' resource) in MacBinary form, ready
for hfsutils' "hcopy -m".

    python3 peflink.py -o Hello.bin hello.o [more.o ...] [lib.a ...]
        [--imports imports.json] [--carbon] [--name Hello]
        [--creator ????] [--size KB] [--pef out.pef] [--entry main]

imports.json comes from gen-imports.py.

For C++, a strong definition replaces weak ones (inline functions,
templates and type information are weak in XCOFF), and the linker
removes the csects that nothing reaches from the entry point and the
constructor and destructor tables (--no-gc-sections keeps them all), so
of the copies of a weak function only the one in use stays. Compile with
-ffunction-sections and -fdata-sections to make this remove most. A word
that is the difference of two symbols (R_POS and R_NEG at the same
address, as in the traceback tables and the exception tables) is
computed here, since it needs no loader fixup.
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macres import cfrg, file_type, macbinary, resource_fork, size_res  # noqa: E402
import archive  # noqa: E402

# ---------------------------------------------------------------- XCOFF

STYP_TEXT, STYP_DATA, STYP_BSS = 0x20, 0x40, 0x80
C_EXT, C_HIDEXT, C_WEAKEXT = 2, 107, 111
XTY_ER, XTY_SD, XTY_LD, XTY_CM = 0, 1, 2, 3
XMC_PR, XMC_RO, XMC_DB, XMC_TC, XMC_UA, XMC_RW, XMC_GL = 0, 1, 2, 3, 4, 5, 6
XMC_XO, XMC_SV, XMC_BS, XMC_DS, XMC_UC = 7, 8, 9, 10, 11
XMC_TC0, XMC_TD, XMC_TE = 15, 16, 22
R_POS, R_NEG, R_REL, R_TOC, R_TRL, R_TRLA = 0x00, 0x01, 0x02, 0x03, 0x12, 0x13
R_BA, R_BR, R_RBA, R_RBR = 0x08, 0x0A, 0x18, 0x1A

CODE_CLASSES = {XMC_PR, XMC_RO, XMC_DB, XMC_GL, XMC_XO, XMC_SV}
TOC_CLASSES = {XMC_TC, XMC_TD, XMC_TC0, XMC_TE}

NOP = 0x60000000
LWZ_R2_20_R1 = 0x80410014


class LinkError(Exception):
    """An error in the input objects, which the linker reports and stops."""
    pass


class Csect:
    """One control section (csect) of an XCOFF object, as the linker places it."""
    def __init__(self, obj, sym, name, addr, size, smtyp, smclas, align,
                 sect):
        """Keep the csect's symbol, name, address, size, type, class, alignment
        and section."""
        self.obj, self.sym, self.name = obj, sym, name
        self.addr, self.size = addr, size
        self.smtyp, self.smclas, self.align = smtyp, smclas, align
        self.sect = sect          # XCOFF section header dict, or None (CM)
        self.relocs = []          # (vaddr, symndx, rsize, rtype)
        self.out = None           # 'code' | 'data' | 'bss'
        self.pos = None           # offset in that PEF section
        self.live = False

    def data(self):
        """Give the contents of the csect (zeros for BSS and common symbols)."""
        if self.sect is None or self.sect["flags"] & STYP_BSS:
            return bytes(self.size)
        off = self.addr - self.sect["vaddr"]
        raw = self.sect["raw"]
        return raw[off:off + self.size]


class Symbol:
    """One symbol table entry of an XCOFF object."""
    def __init__(self, idx, name, value, scnum, sclass, csect=None,
                 smtyp=None, smclas=None):
        """Keep the symbol's index, name, value, section, storage class and
        csect."""
        self.idx, self.name, self.value = idx, name, value
        self.scnum, self.sclass = scnum, sclass
        self.csect, self.smtyp, self.smclas = csect, smtyp, smclas


class XcoffObject:
    """One XCOFF relocatable object: its sections, csects, symbols and
    relocations."""
    def __init__(self, path, blob):
        """Read the sections, csects, symbols and relocations of an XCOFF
        object."""
        self.path = path
        magic, nscns, _, symptr, nsyms, opthdr, _ = struct.unpack_from(
            ">HHiiiHH", blob, 0)
        if magic != 0x01DF:
            raise LinkError(f"{path}: not a 32-bit XCOFF object")
        self.sections = []
        off = 20 + opthdr
        for _ in range(nscns):
            (name, paddr, vaddr, size, scnptr, relptr, lnnoptr, nreloc,
             nlnno, flags) = struct.unpack_from(">8sIIIIIIHHI", blob, off)
            raw = b"" if flags & STYP_BSS else blob[scnptr:scnptr + size]
            relocs = []
            for i in range(nreloc):
                relocs.append(struct.unpack_from(">IIBB", blob,
                                                 relptr + 10 * i))
            self.sections.append(dict(name=name.rstrip(b"\0").decode(),
                                      vaddr=vaddr, size=size, flags=flags,
                                      raw=raw, relocs=relocs))
            off += 40
        strtab = blob[symptr + 18 * nsyms:]

        def symname(raw8):
            """Read a symbol name: inline in its 8 bytes, or at an offset in
            the string table."""
            if raw8[:4] == b"\0\0\0\0":
                o = struct.unpack(">I", raw8[4:])[0]
                e = strtab.index(b"\0", o)
                return strtab[o:e].decode("mac_roman")
            return raw8.rstrip(b"\0").decode("mac_roman")

        self.symbols = {}
        self.csects = []
        i = 0
        while i < nsyms:
            base = symptr + 18 * i
            raw8 = blob[base:base + 8]
            value, scnum, _, sclass, numaux = struct.unpack_from(
                ">IhHBB", blob, base + 8)
            name = symname(raw8)
            sym = Symbol(i, name, value, scnum, sclass)
            if sclass in (C_EXT, C_HIDEXT, C_WEAKEXT) and numaux:
                aux = symptr + 18 * (i + numaux)
                scnlen, _, _, smtyp, smclas = struct.unpack_from(
                    ">IIHBB", blob, aux)
                sym.smtyp, sym.smclas = smtyp & 7, smclas
                kind = smtyp & 7
                if kind in (XTY_SD, XTY_CM):
                    sect = self.sections[scnum - 1] if scnum > 0 else None
                    cs = Csect(self, sym, name, value, scnlen, kind, smclas,
                               smtyp >> 3, sect)
                    sym.csect = cs
                    self.csects.append(cs)
                elif kind == XTY_LD:
                    sym.csect = self.symbols[scnlen].csect
            self.symbols[i] = sym
            i += 1 + numaux

        # Give every relocation to the csect that holds its address. Only
        # the loaded sections (.text, .data and .bss) go in the PEF
        # container. Ignore the relocations of the other sections, such as
        # the DWARF debug sections that -g makes (STYP_DWARF): PEF has no
        # place for debug information, so the linker discards it.
        for sect in self.sections:
            if not sect["flags"] & (STYP_TEXT | STYP_DATA | STYP_BSS):
                continue
            # An empty csect (the TOC anchor) shares its address with the
            # one after it, which owns the relocation: try sized ones first.
            owners = sorted((c for c in self.csects if c.sect is sect),
                            key=lambda c: (c.addr, c.size == 0))
            for r in sect["relocs"]:
                for c in owners:
                    if c.addr <= r[0] < c.addr + max(c.size, 1):
                        c.relocs.append(r)
                        break
                else:
                    raise LinkError(f"{path}: relocation at {r[0]:#x} is "
                                    f"outside every csect")


def defined_names(o):
    """Give the global names that object o defines."""
    return {s.name for s in o.symbols.values()
            if s.sclass in (C_EXT, C_WEAKEXT) and s.csect is not None
            and s.smtyp in (XTY_SD, XTY_LD, XTY_CM)}


def referenced_names(o):
    """Give the global names that object o uses and does not define."""
    return {s.name for s in o.symbols.values()
            if s.sclass in (C_EXT, C_WEAKEXT) and s.smtyp == XTY_ER}


def read_inputs(paths, entry="main"):
    """Read the objects, then the archive members they need (before the
    shared libraries, so a library can stand in for an import)."""
    objs, libs = [], []
    for p in paths:
        blob = open(p, "rb").read()
        if archive.is_archive(blob):
            libs.append((p, blob))
        else:
            objs.append(XcoffObject(p, blob))
    return objs + archive.select(objs, libs, XcoffObject, defined_names,
                                 referenced_names, {entry})


# ---------------------------------------------------------------- imports

class Imports:
    """The shared library functions and data that programs can import."""
    def __init__(self, path, carbon):
        """Read the shared library exports from path (with CarbonLib's when
        carbon is true)."""
        libs = json.load(open(path, encoding="utf-8"))
        self.where = {}
        # InterfaceLib first so it wins names that other libraries repeat,
        # and Apple's private libraries (names like "Apple;Carbon;Networking",
        # which re-export public calls) last.
        order = sorted(libs, key=lambda n: (n != "InterfaceLib", ";" in n, n))
        if carbon:
            order = ["CarbonLib"] + [n for n in order if n != "CarbonLib"]
        else:
            order = [n for n in order if n != "CarbonLib"]
        for lib in order:
            weak = libs[lib].get("weak", False)
            for n in libs[lib]["func"]:
                self.where.setdefault(n, (lib, "func", weak))
            for n in libs[lib]["data"]:
                self.where.setdefault(n, (lib, "data", weak))
        self.used = {}            # name -> import index
        self.order = []           # [(lib, name, kind, weak)]

    def get(self, name):
        """Give the import entry for name, adding it to the used imports, or
        None if no library exports it."""
        if name in self.used:
            return self.used[name]
        if name not in self.where:
            return None
        lib, kind, weak = self.where[name]
        self.used[name] = len(self.order)
        self.order.append((lib, name, kind, weak))
        return self.used[name]


# ---------------------------------------------------------------- linking

def align_up(x, a):
    """Round x up to a multiple of a (a power of two)."""
    return (x + a - 1) & ~(a - 1)


GLUE = [0x81820000, 0x90410014, 0x800C0000, 0x804C0004, 0x7C0903A6,
        0x4E800420]


IMPORT_PREFIX = "__imp_"


def import_name(name):
    """Give the library name of a Toolbox reference, or None.

    On PowerPC the Mac OS headers give each Toolbox function the
    assembler name __imp_X (.__imp_X for its code). Thus a program
    function with the same C name (a Win32 ShowWindow, for example)
    does not replace the Toolbox function X.
    """
    bare = name[1:] if name.startswith(".") else name
    if bare.startswith(IMPORT_PREFIX):
        return bare[len(IMPORT_PREFIX):]
    return None


def make_import_target(globals_, imports):
    """Give a function that maps a Toolbox reference to a program global.

    A reference to __imp_X goes to a program definition of __imp_X if
    there is one. Else it goes to the import X if a library exports X.
    Else it goes to a program definition of X (a function that the SDK
    supplies in place of the import). The function gives that global
    name, or None when the reference goes to an import or to nothing.
    """
    def import_target(name):
        """Give the global name that name resolves to, or None."""
        lib = import_name(name)
        if lib is None or name in globals_ or lib in imports.where:
            return None
        alt = ("." if name.startswith(".") else "") + lib
        return alt if alt in globals_ else None
    return import_target


def collect_garbage(objs, globals_, roots, gc, import_target):
    """Mark the csects the program reaches; with gc, drop the others.

    Each object's csects are kept in its csect list only when live.
    References by name go to the definition in globals_, so of the
    copies of a weak csect only the chosen one is reached.
    """
    work = []

    def mark(cs):
        """Mark csect cs live and queue it for the scan of its relocations."""
        if cs is not None and not cs.live:
            cs.live = True
            work.append(cs)

    for o in objs:
        for c in o.csects:
            # Without gc, and for the TOC anchor, keep everything.
            if not gc or c.smclas == XMC_TC0:
                mark(c)
    for cs in roots:
        mark(cs)
    while work:
        c = work.pop()
        for (_, symndx, _, _) in c.relocs:
            s = c.obj.symbols[symndx]
            if s.csect is not None and s.sclass == C_HIDEXT:
                mark(s.csect)
            elif s.name in globals_:
                mark(globals_[s.name][0])
            elif import_target(s.name) in globals_:
                mark(globals_[import_target(s.name)][0])
            elif s.csect is not None:
                mark(s.csect)
    for o in objs:
        o.csects = [c for c in o.csects if c.live]


def link(objs, imports, entry="main", gc=True):
    """Place the live csects of objs in code and data, apply the relocations,
    and give the image, its imports and its fixups."""
    # Global symbol table: name -> (csect, offset-in-csect, class). A
    # strong (C_EXT) definition replaces a weak (C_WEAKEXT) one; a common
    # symbol gives way to a definition.
    globals_ = {}
    for o in objs:
        for s in o.symbols.values():
            if s.sclass in (C_EXT, C_WEAKEXT) and s.csect is not None \
                    and s.smtyp in (XTY_SD, XTY_LD, XTY_CM):
                old = globals_.get(s.name)
                if old is not None:
                    if s.sclass == C_WEAKEXT or s.smtyp == XTY_CM:
                        continue
                    if old[2] == C_EXT and old[0].smtyp != XTY_CM:
                        raise LinkError(f"duplicate symbol {s.name}")
                globals_[s.name] = (s.csect, s.value - s.csect.addr,
                                    s.sclass)

    # The C library walks constructor and destructor tables between
    # __init_array_start and __init_array_end (and so on). clang names
    # each XCOFF object's constructors and destructors as AIX does, with
    # a global function __sinitPPPPPPPP_... (or __sterm...), PPPPPPPP
    # being the priority in hex, lowest first; the tables here hold
    # pointers to their descriptors in that order. The C library runs
    # the destructor table backwards.
    def by_priority(prefix):
        found = []
        for name, (cs, off, _) in globals_.items():
            if name.startswith(prefix) and cs.smclas == XMC_DS:
                try:
                    prio = int(name[len(prefix):len(prefix) + 8], 16)
                except ValueError:
                    continue
                found.append((prio, len(found), cs, off))
        return [(cs, off) for _, _, cs, off in sorted(found)]
    inits, finis = by_priority("__sinit"), by_priority("__sterm")
    roots = [cs for cs, _ in inits + finis]
    if entry in globals_:
        roots.append(globals_[entry][0])
    import_target = make_import_target(globals_, imports)
    collect_garbage(objs, globals_, roots, gc, import_target)
    tables = Csect(None, None, "__array_tables", 0,
                   4 * (len(inits) + len(finis)), XTY_SD, XMC_RW, 2, None)
    ni, nf = 4 * len(inits), 4 * (len(inits) + len(finis))
    for name, off in (("preinit_array_start", 0), ("preinit_array_end", 0),
                      ("init_array_start", 0), ("init_array_end", ni),
                      ("bothinit_array_start", 0), ("bothinit_array_end", ni),
                      ("fini_array_start", ni), ("fini_array_end", nf)):
        globals_.setdefault(f"__{name}", (tables, off, C_EXT))

    code, data_head, toc, bss = [], [tables], [], []
    for o in objs:
        for c in o.csects:
            if c.smtyp == XTY_CM or c.smclas in (XMC_BS, XMC_UC):
                bss.append(c)
            elif c.smclas in CODE_CLASSES and c.sect["flags"] & STYP_TEXT:
                code.append(c)
            elif c.smclas in TOC_CLASSES:
                toc.append(c)
            else:
                data_head.append(c)

    # Code section: csects, then glue.
    pos = 0
    for c in code:
        pos = align_up(pos, 1 << c.align)
        c.out, c.pos = "code", pos
        pos += c.size
    glue_base = align_up(pos, 4)

    # Data section: ordinary data, the TOC (TC0 anchors first), then bss.
    pos = 0
    for c in data_head:
        pos = align_up(pos, 1 << c.align)
        c.out, c.pos = "data", pos
        pos += c.size
    pos = align_up(pos, 4)
    toc_base = pos
    toc.sort(key=lambda c: c.smclas != XMC_TC0)
    for c in toc:
        c.out, c.pos = "data", pos
        pos += c.size

    # Work out imports: which functions need glue, which TOC slots.
    def resolve(o, symndx):
        s = o.symbols[symndx]
        # A global name goes to the chosen definition, which may be
        # another object's copy of a weak csect.
        if s.csect is not None and (s.sclass == C_HIDEXT or
                                    s.name not in globals_):
            return ("csect", s.csect, s.value - s.csect.addr)
        name = import_target(s.name) or s.name
        if name in globals_:
            cs, off, _ = globals_[name]
            return ("csect", cs, off)
        if name.startswith(".") and name[1:] in globals_:
            raise LinkError(f"{o.path}: {name} has no code entry point")
        bare = name[1:] if name.startswith(".") else name
        bare = import_name(bare) or bare
        idx = imports.get(bare)
        if idx is None:
            if s.sclass == C_WEAKEXT:
                return ("null",)
            raise LinkError(f"{o.path}: undefined symbol {name}")
        return ("import", idx, name.startswith("."))

    glue_of = {}       # import index -> code offset of its glue
    glue_slot = {}     # import index -> data offset of the TOC slot
    for o in objs:
        for c in o.csects:
            for (_, symndx, _, rtype) in c.relocs:
                if rtype in (R_BR, R_RBR):
                    r = resolve(o, symndx)
                    if r[0] == "import" and r[1] not in glue_of:
                        glue_of[r[1]] = None
    for idx in sorted(glue_of):
        glue_slot[idx] = pos
        pos += 4
    for n, idx in enumerate(sorted(glue_of)):
        glue_of[idx] = glue_base + 24 * n
    code_size = glue_base + 24 * len(glue_of)
    if pos - toc_base > 0x7FFF:
        raise LinkError("TOC is larger than 32 KiB")

    data_init = pos
    for c in bss:
        pos = align_up(pos, 1 << c.align)
        c.out, c.pos = "bss", pos
        pos += c.size
    data_size = align_up(pos, 4)

    code_img = bytearray(code_size)
    data_img = bytearray(data_init)
    for c in code:
        code_img[c.pos:c.pos + c.size] = c.data()
    for c in data_head + toc:
        data_img[c.pos:c.pos + c.size] = c.data()
    for i, (cs, off) in enumerate(inits + finis):
        at = tables.pos + 4 * i
        struct.pack_into(">I", data_img, at, cs.pos + off)

    # Loader fixups for words in the data section:
    # data offset -> ('C' | 'D' | ('I', idx))
    fixups = {}

    def sect_addr(cs, off):
        """Give the section (C for code, D for data) and the address of offset
        off in csect cs."""
        kind = "C" if cs.out == "code" else "D"
        return kind, cs.pos + off

    for i in range(len(inits) + len(finis)):
        fixups[tables.pos + 4 * i] = ("D",)
    for idx, slot in glue_slot.items():
        fixups[slot] = ("I", idx)
        g = glue_of[idx]
        words = list(GLUE)
        words[0] |= (slot - toc_base) & 0xFFFF
        struct.pack_into(">6I", code_img, g, *words)

    def address(r):
        """Return (kind, offset) of a resolved symbol: 'C' or 'D'."""
        cs, soff = r[1], r[2]
        if cs.out == "bss":
            return "D", cs.pos + soff
        return sect_addr(cs, soff)

    for o in objs:
        for c in o.csects:
            if c.out not in ("code", "data"):
                continue
            img = code_img if c.out == "code" else data_img
            # Words that are a difference of two symbols: an R_POS and
            # an R_NEG at the same address.
            negs = {vaddr: symndx for (vaddr, symndx, rsize, rtype)
                    in c.relocs if rtype == R_NEG}
            for (vaddr, symndx, rsize, rtype) in c.relocs:
                off = vaddr - c.addr
                at = c.pos + off
                length = (rsize & 0x3F) + 1
                r = resolve(o, symndx)
                orig_sym = o.symbols[symndx]
                if rtype == R_NEG and length == 32:
                    continue                # with its R_POS
                if rtype == R_POS and length == 32 and vaddr in negs:
                    n = resolve(o, negs[vaddr])
                    nsym = o.symbols[negs[vaddr]]
                    if r[0] != "csect" or n[0] != "csect":
                        raise LinkError(f"{o.path}: difference of an "
                                        f"import in {c.name}+{off:#x}")
                    (pk, pa), (nk, na) = address(r), address(n)
                    if pk != nk:
                        raise LinkError(f"{o.path}: difference of symbols "
                                        f"in two sections in {c.name}")
                    old = struct.unpack_from(">I", img, at)[0]
                    addend = old - (orig_sym.value - nsym.value)
                    struct.pack_into(">I", img, at,
                                     (pa - na + addend) & 0xFFFFFFFF)
                    continue
                if r[0] == "null":
                    if rtype == R_POS and length == 32 and c.out == "data":
                        struct.pack_into(">I", img, at, 0)
                        continue
                    # A call to an undefined weak function: the code
                    # tests the function's address (a TOC word, 0 here)
                    # first, so the call never runs. The branch stays as
                    # the compiler wrote it.
                    if rtype in (R_BR, R_RBR) and length == 26:
                        continue
                    raise LinkError(f"{o.path}: undefined weak symbol "
                                    f"{orig_sym.name} used in {c.name}")
                if rtype == R_POS and length == 32:
                    if c.out != "data":
                        raise LinkError(f"{o.path}: absolute pointer in "
                                        f"code ({c.name}+{off:#x})")
                    old = struct.unpack_from(">I", img, at)[0]
                    if r[0] == "import":
                        if r[2]:
                            raise LinkError(f"{o.path}: pointer to code "
                                            f"of import {orig_sym.name}")
                        struct.pack_into(">I", img, at, old)
                        fixups[at] = ("I", r[1])
                    else:
                        cs, soff = r[1], r[2]
                        if cs.out == "bss":
                            kind, target = "D", cs.pos + soff
                        else:
                            kind, target = sect_addr(cs, soff)
                        # The stored word is S + A with S the symbol's
                        # value in this object's symbol table (also for a
                        # symbol it only refers to); keep A.
                        addend = old - orig_sym.value
                        struct.pack_into(">I", img, at,
                                         (target + addend) & 0xFFFFFFFF)
                        fixups[at] = (kind,)
                elif rtype in (R_TOC, R_TRL, R_TRLA) and length == 16:
                    if r[0] != "csect" or r[1].out != "data":
                        raise LinkError(f"{o.path}: TOC reference to "
                                        f"{orig_sym.name} outside the TOC")
                    target = r[1].pos + r[2] - toc_base
                    if not -0x8000 <= target < 0x8000:
                        raise LinkError("TOC offset out of range")
                    struct.pack_into(">h", img, at, target)
                elif rtype in (R_BR, R_RBR) and length == 26:
                    word = struct.unpack_from(">I", img, at)[0]
                    if r[0] == "import":
                        dest = glue_of[r[1]]
                        nxt = struct.unpack_from(">I", img, at + 4)[0]
                        if nxt == NOP:
                            struct.pack_into(">I", img, at + 4, LWZ_R2_20_R1)
                        elif nxt != LWZ_R2_20_R1:
                            raise LinkError(f"{o.path}: call to "
                                            f"{orig_sym.name} at {c.name}+"
                                            f"{off:#x} has no nop after it")
                    else:
                        cs, soff = r[1], r[2]
                        if cs.out != "code":
                            raise LinkError(f"{o.path}: branch to data "
                                            f"{orig_sym.name}")
                        dest = cs.pos + soff
                    disp = dest - at
                    if not -0x2000000 <= disp < 0x2000000:
                        raise LinkError("branch out of range")
                    word = (word & 0xFC000003) | (disp & 0x03FFFFFC)
                    struct.pack_into(">I", img, at, word)
                elif rtype == R_REL and length == 32 and c.out == "data":
                    raise LinkError("R_REL in data is not supported")
                else:
                    raise LinkError(f"{o.path}: unsupported relocation "
                                    f"type {rtype:#x} size {length} in "
                                    f"{c.name}")

    if entry not in globals_:
        raise LinkError(f"no {entry}")
    mcs, moff, _ = globals_[entry]
    if mcs.out != "data" or mcs.smclas != XMC_DS:
        raise LinkError(f"{entry} has no function descriptor")
    main_off = mcs.pos + moff
    return dict(code=bytes(code_img), data=bytes(data_img),
                data_size=data_size, fixups=fixups, main=main_off,
                toc=toc_base)


# ---------------------------------------------------------------- PEF

def reloc_instrs(fixups):
    """Encode data-section fixups as PEF relocation instructions."""
    out = []
    pos = 0
    items = sorted(fixups.items())
    i = 0
    while i < len(items):
        at, fx = items[i]
        if at & 3:
            raise LinkError(f"unaligned fixup at data+{at:#x}")
        if at != pos:
            gap = at - pos
            if gap < 0:
                raise LinkError("overlapping fixups")
            if gap <= 4096:
                out.append(0x8000 | (gap - 1))
            else:
                if at >= 1 << 26:
                    raise LinkError("data section too large")
                out += [0xA000 | (at >> 16), at & 0xFFFF]
            pos = at
        # Group a run of words with the same kind.
        if fx[0] in ("C", "D"):
            n = 1
            while (i + n < len(items) and items[i + n][0] == at + 4 * n and
                   items[i + n][1] == fx and n < 512):
                n += 1
            out.append((0x4000 if fx[0] == "C" else 0x4200) | (n - 1))
        else:
            n = 1
            idx = fx[1]
            if idx >= 512:
                raise LinkError("more than 512 imports")
            out.append(0x6000 | idx)
        pos += 4 * n
        i += n
    return out


def build_pef(img, imports):
    """Make the PEF container (code, data and loader sections) from the linked
    image."""
    code, data = img["code"], img["data"]

    # Loader strings: libraries, then symbols.
    strings = bytearray()

    def add_str(s):
        """Add s to the loader string table and give its offset."""
        o = len(strings)
        strings.extend(s.encode("mac_roman") + b"\0")
        return o

    libs = []
    for lib in dict.fromkeys(l for (l, _, _, _) in imports.order):
        libs.append(lib)
    # PEF wants the imported symbols grouped by library, in the same
    # order as the import indexes; renumber so that holds.
    remap, syms = {}, []
    for lib in libs:
        for old, (l, name, kind, weak) in enumerate(imports.order):
            if l == lib:
                remap[old] = len(syms)
                syms.append((l, name, kind, weak))
    fixups = {at: (fx if fx[0] != "I" else ("I", remap[fx[1]]))
              for at, fx in img["fixups"].items()}

    lib_entries = []
    for lib in libs:
        first = next(i for i, s in enumerate(syms) if s[0] == lib)
        count = sum(1 for s in syms if s[0] == lib)
        weak = syms[first][3]
        lib_entries.append((add_str(lib), first, count, weak))
    sym_words = []
    for (_, name, kind, weak) in syms:
        cls = 2 if kind == "func" else 1          # TVector / data
        if weak:
            cls |= 0x80
        sym_words.append((cls << 24) | add_str(name))

    relocs = reloc_instrs(fixups)

    hdr_size = 56
    libs_off = hdr_size
    syms_off = libs_off + 24 * len(lib_entries)
    rhdr_off = syms_off + 4 * len(sym_words)
    rinstr_off = rhdr_off + (12 if relocs else 0)
    strings_off = rinstr_off + 2 * len(relocs)
    hash_off = align_up(strings_off + len(strings), 4)

    loader = bytearray(hash_off + 4)
    struct.pack_into(">iIiIiIIIIIIIII", loader, 0,
                     1, img["main"], -1, 0, -1, 0,
                     len(lib_entries), len(sym_words),
                     1 if relocs else 0, rinstr_off, strings_off, hash_off,
                     0, 0)
    for n, (name_off, first, count, weak) in enumerate(lib_entries):
        struct.pack_into(">IIIIIBBH", loader, libs_off + 24 * n, name_off,
                         0, 0, count, first, 0x40 if weak else 0, 0, 0)
    for n, w in enumerate(sym_words):
        struct.pack_into(">I", loader, syms_off + 4 * n, w)
    if relocs:
        struct.pack_into(">HHII", loader, rhdr_off, 1, 0, len(relocs), 0)
        for n, r in enumerate(relocs):
            struct.pack_into(">H", loader, rinstr_off + 2 * n, r)
    loader[strings_off:strings_off + len(strings)] = strings
    # One empty export hash bucket: no exports.

    sections = [
        # (body, total, unpacked, kind, share, align)
        (code, len(code), len(code), 0, 4, 4),
        (data, img["data_size"], len(data), 1, 1, 4),
        (bytes(loader), 0, len(loader), 4, 4, 4),
    ]
    out = bytearray(40 + 28 * len(sections))
    struct.pack_into(">4s4s4sIIIIIHHI", out, 0, b"Joy!", b"peff", b"pwpc",
                     1, 0, 0, 0, 0, len(sections), 2, 0)
    for n, (body, total, unpacked, kind, share, al) in enumerate(sections):
        out.extend(bytes(align_up(len(out), 16) - len(out)))
        off = len(out)
        out.extend(body)
        struct.pack_into(">iIIIIIBBBB", out, 40 + 28 * n, -1, 0,
                         total if kind != 4 else len(body), unpacked, len(body), off,
                         kind, share, al, 0)
    return bytes(out)


# ---------------------------------------------------------------- main

def main():
    """Read the command line, link the objects and write the application."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("-o", dest="output", required=True,
                    help="MacBinary application to write")
    ap.add_argument("--imports", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "imports.json"))
    ap.add_argument("--carbon", action="store_true",
                    help="import from CarbonLib before InterfaceLib")
    ap.add_argument("--name", help="application name (default: output)")
    ap.add_argument("--creator", default="????")
    ap.add_argument("--size", type=int, default=2048,
                    help="memory partition in KiB")
    ap.add_argument("--type", default="APPL",
                    help="file type (default APPL)")
    ap.add_argument("--background-only", action="store_true",
                    help="set onlyBackground in the SIZE resource (an "
                    "application without a user interface)")
    ap.add_argument("--pef", help="also write the bare PEF container")
    ap.add_argument("--entry", default="main",
                    help="the function the Code Fragment Manager calls")
    ap.add_argument("--no-gc-sections", dest="gc", action="store_false",
                    help="keep the csects that nothing refers to")
    a = ap.parse_args()

    try:
        objs = read_inputs(a.inputs, a.entry)
        imports = Imports(a.imports, a.carbon)
        img = link(objs, imports, a.entry, a.gc)
        pef = build_pef(img, imports)
    except (LinkError, archive.ArchiveError) as e:
        sys.exit(f"peflink: error: {e}")

    name = a.name or os.path.splitext(os.path.basename(a.output))[0]
    creator = file_type(a.creator)
    rsrc = resource_fork([(b"cfrg", 0, cfrg(name)),
                          (b"SIZE", -1, size_res(a.size, a.background_only))])
    if a.pef:
        open(a.pef, "wb").write(pef)
    open(a.output, "wb").write(macbinary(name, file_type(a.type), creator, pef, rsrc))
    libs = sorted({l for (l, _, _, _) in imports.order})
    print(f"peflink: {a.output}: code {len(img['code'])}, data "
          f"{img['data_size']}, {len(imports.order)} imports from "
          f"{', '.join(libs) or 'nothing'}")


if __name__ == "__main__":
    main()
