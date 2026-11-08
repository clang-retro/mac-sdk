"""Static libraries (ar archives) for codelink.py and peflink.py.

Reads the GNU and BSD ar formats (what llvm-ar writes for ELF objects)
and AIX's big archive format (what it writes for XCOFF objects), and picks the members a program needs, the way a Unix
linker does: a member is linked when it defines a symbol that the
objects linked so far reference and nothing defines, until no more
members are needed. Archives are searched together, so members may
refer to each other in any order.
"""


class ArchiveError(Exception):
    pass


def is_archive(blob):
    return blob[:8] in (b"!<arch>\n", b"<bigaf>\n")


def big_members(path, blob):
    """AIX big archive: a fixed header, then members in a linked list."""
    def num(off, n):
        return int(blob[off:off + n].strip(b" \0") or b"0")
    out = []
    pos = num(68, 20)                     # first member
    memtab = num(8, 20)
    seen = set()
    while pos and pos != memtab and pos not in seen:
        seen.add(pos)
        if pos + 112 > len(blob):
            raise ArchiveError(f"{path}: bad member offset {pos:#x}")
        size = num(pos, 20)
        nxt = num(pos + 20, 20)
        namlen = num(pos + 108, 4)
        name = blob[pos + 112:pos + 112 + namlen]
        data = pos + 112 + namlen + (namlen & 1) + 2   # name, pad, "`\n"
        out.append((name.decode("latin-1"), blob[data:data + size]))
        pos = nxt
    return out


def members(path, blob):
    """Return [(member name, bytes)] for every object in the archive."""
    if blob[:8] == b"<bigaf>\n":
        return big_members(path, blob)
    if not is_archive(blob):
        raise ArchiveError(f"{path}: not an ar archive")
    out = []
    longnames = b""
    pos = 8
    while pos + 60 <= len(blob):
        hdr = blob[pos:pos + 60]
        if hdr[58:60] != b"`\n":
            raise ArchiveError(f"{path}: bad member header at {pos:#x}")
        name = hdr[:16].rstrip(b" ")
        size = int(hdr[48:58].strip() or b"0")
        body = blob[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if name in (b"/", b"/SYM64/", b"__.SYMDEF", b"__.SYMDEF SORTED"):
            continue                      # symbol table: we read the members
        if name == b"//":
            longnames = body
            continue
        if name.startswith(b"#1/"):       # BSD: name before the data
            n = int(name[3:])
            name, body = body[:n].rstrip(b"\0"), body[n:]
        elif name.startswith(b"/") and name[1:].isdigit():
            o = int(name[1:])
            end = longnames.index(b"\n", o)
            name = longnames[o:end].rstrip(b"/")
        else:
            name = name.rstrip(b"/")
        out.append((name.decode("latin-1"), body))
    return out


def select(objs, archives, load, defines, references, extra=()):
    """Add to objs the archive members that resolve its undefined symbols.

    archives: [(path, blob)]. load(label, blob) parses one member;
    defines(obj) and references(obj) give the global names an object
    defines and refers to; extra names more symbols to resolve, such as
    the entry point. Returns the members added, in link order.
    """
    pool = []
    for path, blob in archives:
        for name, body in members(path, blob):
            pool.append([f"{path}({name})", body, None])
    defined, wanted = set(), set(extra)
    for o in objs:
        defined |= defines(o)
        wanted |= references(o)
    added = []
    changed = True
    while changed:
        changed = False
        need = wanted - defined
        if not need:
            break
        for m in pool:
            if m[2] is False:
                continue                  # already linked
            if m[2] is None:
                m[2] = load(m[0], m[1])
            obj = m[2]
            if not defines(obj) & need:
                continue
            added.append(obj)
            defined |= defines(obj)
            wanted |= references(obj)
            m[2] = False
            changed = True
            need = wanted - defined
    return added
