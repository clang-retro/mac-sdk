#!/usr/bin/env python3
"""Read and write files on a 400K MFS floppy image (raw, not DiskCopy).

    mfs.py ls IMG
    mfs.py get IMG NAME OUT          (data fork)
    mfs.py rm IMG NAME
    mfs.py put IMG NAME APP.bin      (a MacBinary file: both forks, type, creator)
"""
import struct
import sys

SECTOR = 512


class MFS:
    def __init__(self, path):
        self.path = path
        self.d = bytearray(open(path, 'rb').read())
        m = 0x400
        (sig, self.crdate, _, _, self.nfiles, self.dirst, self.dirlen, self.nblocks,
         self.blksize, _, self.alst, self.nextfnum, self.free) = struct.unpack_from(
             '>HIIHHHHHIIHIH', self.d, m)
        assert sig == 0xD2D7, 'not an MFS volume'
        self.mapoff = m + 64

    # 12-bit allocation map entries, blocks numbered from 2
    def get_map(self, n):
        i = n - 2
        o = self.mapoff + i * 3 // 2
        w = (self.d[o] << 8) | self.d[o + 1]
        return (w >> 4) if i % 2 == 0 else (w & 0xFFF)

    def set_map(self, n, v):
        i = n - 2
        o = self.mapoff + i * 3 // 2
        if i % 2 == 0:
            self.d[o] = v >> 4
            self.d[o + 1] = ((v & 15) << 4) | (self.d[o + 1] & 15)
        else:
            self.d[o] = (self.d[o] & 0xF0) | (v >> 8)
            self.d[o + 1] = v & 0xFF

    def entries(self):
        for s in range(self.dirst, self.dirst + self.dirlen):
            o = s * SECTOR
            end = o + SECTOR
            while o < end - 51 and self.d[o] & 0x80:
                nl = self.d[o + 50]
                yield o, bytes(self.d[o + 51:o + 51 + nl])
                o += (51 + nl + 1) & ~1

    def chain(self, start):
        out = []
        n = start
        while n and n != 1:
            out.append(n)
            n = self.get_map(n)
        return out

    def fork(self, start, length):
        b = b''.join(self.block(n) for n in self.chain(start)) if start else b''
        return b[:length]

    def block(self, n):
        o = self.alst * SECTOR + (n - 2) * self.blksize
        return self.d[o:o + self.blksize]

    def find(self, name):
        if isinstance(name, str):
            name = name.encode('mac_roman')
        for o, nm in self.entries():
            if nm.lower() == name.lower():
                return o
        return None

    def ls(self):
        for o, nm in self.entries():
            t, c = self.d[o + 2:o + 6], self.d[o + 6:o + 10]
            dl, rl = struct.unpack_from('>I', self.d, o + 24)[0], struct.unpack_from('>I', self.d, o + 34)[0]
            print(f"{t.decode('mac_roman')}/{c.decode('mac_roman')} {dl:8} {rl:8} {nm.decode('mac_roman')}")

    def rm(self, name):
        o = self.find(name)
        if o is None:
            return False
        for so in (o + 22, o + 32):
            for n in self.chain(struct.unpack_from('>H', self.d, so)[0]):
                self.set_map(n, 0)
                self.free += 1
        # close the gap in this directory sector
        s = o // SECTOR * SECTOR
        size = (51 + self.d[o + 50] + 1) & ~1
        rest = self.d[o + size:s + SECTOR]
        self.d[o:s + SECTOR] = rest + bytes(size)
        self.nfiles -= 1
        return True

    def alloc(self, data):
        if not data:
            return 0, 0
        need = -(-len(data) // self.blksize)
        free = [n for n in range(2, self.nblocks + 2) if self.get_map(n) == 0][:need]
        assert len(free) == need, 'disk full'
        for i, n in enumerate(free):
            self.set_map(n, free[i + 1] if i + 1 < need else 1)
            o = self.alst * SECTOR + (n - 2) * self.blksize
            chunk = data[i * self.blksize:(i + 1) * self.blksize]
            self.d[o:o + len(chunk)] = chunk
        self.free -= need
        return free[0], need * self.blksize

    def put(self, name, ftype, creator, data, rsrc, finder_flags=0):
        self.rm(name)
        ds, dp = self.alloc(data)
        rs, rp = self.alloc(rsrc)
        nm = name.encode('mac_roman')
        size = (51 + len(nm) + 1) & ~1
        for s in range(self.dirst, self.dirst + self.dirlen):
            o = s * SECTOR
            end = o + SECTOR
            while self.d[o] & 0x80:
                o += (51 + self.d[o + 50] + 1) & ~1
            if o + size <= end:
                break
        else:
            raise SystemExit('directory full')
        e = bytearray(size)
        e[0] = 0x80
        e[2:6], e[6:10] = ftype, creator
        struct.pack_into('>H', e, 10, finder_flags)
        struct.pack_into('>IHIIHIIII', e, 18, self.nextfnum, ds, len(data), dp,
                         rs, len(rsrc), rp, self.crdate, self.crdate)
        e[50] = len(nm)
        e[51:51 + len(nm)] = nm
        self.d[o:o + size] = e
        self.nextfnum += 1
        self.nfiles += 1

    def save(self):
        struct.pack_into('>H', self.d, 0x400 + 12, self.nfiles)
        struct.pack_into('>I', self.d, 0x400 + 30, self.nextfnum)
        struct.pack_into('>H', self.d, 0x400 + 34, self.free)
        open(self.path, 'wb').write(self.d)


def macbinary(path):
    b = open(path, 'rb').read()
    dl, rl = struct.unpack_from('>II', b, 83)
    data = b[128:128 + dl]
    r0 = 128 + ((dl + 127) & ~127)
    return b[65:69], b[69:73], data, b[r0:r0 + rl]


def main():
    cmd, img = sys.argv[1], sys.argv[2]
    v = MFS(img)
    if cmd == 'ls':
        v.ls()
    elif cmd == 'get':
        o = v.find(sys.argv[3].encode('mac_roman'))
        ds, dl = struct.unpack_from('>HI', v.d, o + 22)
        open(sys.argv[4], 'wb').write(v.fork(ds, dl))
    elif cmd == 'rm':
        v.rm(sys.argv[3].encode('mac_roman'))
        v.save()
    elif cmd == 'put':
        t, c, data, rsrc = macbinary(sys.argv[4])
        v.put(sys.argv[3], t, c, data, rsrc)
        v.save()


if __name__ == '__main__':
    main()
