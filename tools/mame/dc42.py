"""DiskCopy 4.2 <-> raw. dc42.py raw IN.dc42 OUT.raw | dc42.py pack IN.raw TEMPLATE.dc42 OUT.dc42"""
import struct, sys
def cks(b):
    s = 0
    for i in range(0, len(b), 2):
        s = (s + ((b[i] << 8) | b[i+1])) & 0xFFFFFFFF
        s = ((s >> 1) | ((s & 1) << 31)) & 0xFFFFFFFF
    return s
if sys.argv[1] == 'raw':
    d = open(sys.argv[2], 'rb').read(); n = struct.unpack('>I', d[64:68])[0]
    open(sys.argv[3], 'wb').write(d[84:84+n])
else:
    raw = open(sys.argv[2], 'rb').read(); t = bytearray(open(sys.argv[3], 'rb').read(84))
    struct.pack_into('>II', t, 64, len(raw), 0); struct.pack_into('>II', t, 72, cks(raw), 0)
    open(sys.argv[4], 'wb').write(bytes(t) + raw)
