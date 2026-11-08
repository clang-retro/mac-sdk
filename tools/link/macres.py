"""Mac resource forks and MacBinary files, shared by peflink and codelink."""
import struct
import time


def align_up(x, a):
    return (x + a - 1) & ~(a - 1)


def pstr(s):
    b = s.encode("mac_roman")
    return bytes([len(b)]) + b


def cfrg(name):
    head = struct.pack(">IIHHIIIIHH", 0, 0, 0, 1, 0, 0, 0, 0, 0, 1)
    nm = pstr(name)
    size = align_up(42 + len(nm), 4)
    member = struct.pack(">4sHBBIIIHBBIIIHHH", b"pwpc", 0, 0, 0, 0, 0,
                         0, 0, 1, 1, 0, 0, 0, 0, 0, size) + nm
    return head + member + bytes(size - len(member))


def size_res(kb):
    # canBackground, doesActivateOnFGSwitch, is32BitCompatible,
    # isHighLevelEventAware
    flags = 0x1000 | 0x0800 | 0x0080 | 0x0040
    return struct.pack(">HII", flags, kb * 1024, kb * 1024)


def resource_fork(resources):
    """resources: list of (type, id, bytes)."""
    data = bytearray()
    refs = {}
    for rtype, rid, body in resources:
        refs.setdefault(rtype, []).append((rid, len(data)))
        data += struct.pack(">I", len(body)) + body
    types = list(refs)
    tlist = bytearray(struct.pack(">H", len(types) - 1))
    reflists = bytearray()
    ref_base = 2 + 8 * len(types)
    for t in types:
        tlist += struct.pack(">4sHH", t, len(refs[t]) - 1,
                             ref_base + len(reflists))
        for rid, off in refs[t]:
            reflists += struct.pack(">hhII", rid, -1, off & 0xFFFFFF, 0)
    rmap = bytearray(28) + tlist + reflists
    data_off, map_off = 256, 256 + len(data)
    struct.pack_into(">HH", rmap, 24, 28, len(rmap))
    header = struct.pack(">IIII", data_off, map_off, len(data), len(rmap))
    rmap[:16] = header
    return header + bytes(240) + bytes(data) + bytes(rmap)


def crc16_xmodem(b):
    crc = 0
    for x in b:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def macbinary(name, ftype, creator, dfork, rfork):
    h = bytearray(128)
    nm = name.encode("mac_roman")[:63]
    h[1] = len(nm)
    h[2:2 + len(nm)] = nm
    h[65:69], h[69:73] = ftype, creator
    now = int(time.time()) + 2082844800       # seconds since 1904
    struct.pack_into(">II", h, 83, len(dfork), len(rfork))
    struct.pack_into(">II", h, 91, now & 0xFFFFFFFF, now & 0xFFFFFFFF)
    h[122] = h[123] = 129
    struct.pack_into(">H", h, 124, crc16_xmodem(h[:124]))

    def pad(b):
        return b + bytes(align_up(len(b), 128) - len(b))
    return bytes(h) + pad(dfork) + pad(rfork)


def read_macbinary(blob):
    """Return (name, type, creator, data fork, resource fork)."""
    n = blob[1]
    name = blob[2:2 + n].decode("mac_roman")
    dl, rl = struct.unpack_from(">II", blob, 83)
    roff = 128 + align_up(dl, 128)
    return name, blob[65:69], blob[69:73], blob[128:128 + dl], \
        blob[roff:roff + rl]


def read_resource_fork(rsrc):
    """Return [(type, id, bytes)] for every resource in a resource fork."""
    if len(rsrc) < 16:
        return []
    data_off, map_off = struct.unpack_from(">II", rsrc, 0)
    tlist = map_off + struct.unpack_from(">H", rsrc, map_off + 24)[0]
    out = []
    for i in range(struct.unpack_from(">H", rsrc, tlist)[0] + 1):
        rtype, count, refoff = struct.unpack_from(">4sHH", rsrc,
                                                  tlist + 2 + 8 * i)
        for j in range(count + 1):
            rid, _, ao = struct.unpack_from(">hhI", rsrc,
                                            tlist + refoff + 12 * j)
            off = data_off + (ao & 0xFFFFFF)
            n = struct.unpack_from(">I", rsrc, off)[0]
            out.append((rtype, rid, rsrc[off + 4:off + 4 + n]))
    return out
