#!/usr/bin/env python3
"""List the names that Mac OS shared libraries export, from a Mac OS install.

Every PowerPC shared library on Mac OS 7.5 to 9.2 is a PEF container
named by a 'cfrg' resource: the Extensions folder holds one library per
file, and the System file's data fork holds InterfaceLib and the other
core libraries. This reads each 'cfrg', finds every library fragment,
and reads the export table of its PEF loader section.

    python3 macos-exports.py [--all] imports.json FILE_OR_DIR...

Pass the System Folder (or the System file, the "Mac OS ROM" file and the
Extensions folder). On Mac OS 8.5 and later, InterfaceLib and the other
core libraries live in the ROM image inside the "Mac OS ROM" file, which
this unpacks. On earlier systems they are in the machine's ROM: pass a
4 MB OldWorld ROM image file too.

Files are the data fork; a resource fork is read from "NAME:rsrc" (how
7-Zip extracts HFS+ volumes) or "NAME/..namedfork/rsrc". To get them from
a QEMU disk with Mac OS installed:

    qemu-img convert -O raw hd.qcow2 hd.raw
    7z x hd.raw '*.hfs'           # the Mac OS volume (with HFS wrapper:
                                  # cut out the embedded HFS+ volume first)
    7z x untitled.hfs 'untitled/System Folder/System*' \\
        'untitled/System Folder/Extensions/*'

The output has the same shape as gen-imports.py's:
{"Library": {"func": [...], "data": [...], "weak": bool}}.
"""
import json
import os
import struct
import sys

# Libraries that a program should not need to be present.
WEAK = {"CarbonLib", "Apple;Carbon;Networking"}

# The libraries that Apple's Universal Interfaces 3.4 link against. A
# Mac OS install has hundreds more (drivers, Open Transport modules,
# Java, third-party code), which --all keeps.
PUBLIC = {
    "ATAManager", "ATSUnicodeLib", "AppearanceLib", "Apple;Carbon;Networking",
    "AppleGuideGlueLib", "AppleScriptLib", "CSScriptingLib", "CarbonLib",
    "CfgOpenTpt", "CodeFragmentMgr", "CollectionsLib", "ColorPickerLib",
    "ColorSync", "ContextualMenu", "ControlStripLib", "ControllerCore",
    "ControlsLib", "CursorDevicesLib", "DebugLib", "DictionaryMgrLib",
    "DisplayLib", "DragLib", "DrawSprocketLib", "DriverLoaderLib",
    "DriverServicesLib", "FileSigningLib", "Find by Content", "FoldersLib",
    "FontManager", "FontSyncLib", "HIDLib", "HTMLRenderingLib",
    "ICAp;InternetConfigLib", "IconServicesLib", "ImageCaptureLib",
    "InputSprocketDeferLib", "InputSprocketLib", "InterfaceLib",
    "IsochronousDataHandlerLib", "JManager2", "KeychainLib",
    "LanguageAnalysisMgrLib", "LocalesLib", "MPLibrary", "MacDialogsLib",
    "Math64Lib", "MathLib", "MenusLib", "NSLPPCLib", "NameRegistryLib",
    "NavigationLib", "NetSprocketLib", "OTATalkConfigCreateLib",
    "OTClientLib", "OTClientUtilLib", "OTInetClientLib",
    "OTInetControllerLib", "OTUtilityLib", "OTXTILib", "ObjectSupportLib",
    "OpenGLLibrary", "OpenGLUtility", "PCCard", "PCILib", "PowerMgrLib",
    "QD3DCustomElementsLib", "QD3DViewerLib", "QTStreamLib",
    "QuickDrawGXLib", "QuickDrawText", "QuickDraw™ 3D",
    "QuickDraw™ 3D Accelerator", "QuickTimeLib", "QuickTimeVRLib",
    "SoundLib", "SoundSprocketLib", "SpeechLib", "SpeechRecognitionLib",
    "StartLib", "StdCLib", "TelephoneLib", "TextCommon",
    "TextEncodingConverter", "Textension", "ThreadsLib", "Translation",
    "URLAccessLib", "USBManagerLib", "USBServicesLib", "UTCUtils",
    "UnicodeConverter", "UnicodeUtilitiesCoreLib", "UnicodeUtilitiesLib",
    "VideoServicesLib", "WalkaboutLib", "WindowsLib", "vecLib",
}


def resources(rsrc, want):
    """Return {id: bytes} for resource type `want` in a resource fork."""
    if len(rsrc) < 16:
        return {}
    data_off, map_off, _, _ = struct.unpack_from(">IIII", rsrc, 0)
    tlist = map_off + struct.unpack_from(">H", rsrc, map_off + 24)[0]
    ntypes = struct.unpack_from(">H", rsrc, tlist)[0] + 1
    out = {}
    for i in range(ntypes):
        rtype, count, refoff = struct.unpack_from(">4sHH", rsrc,
                                                  tlist + 2 + 8 * i)
        if rtype != want:
            continue
        for j in range(count + 1):
            rid, _, attr_off = struct.unpack_from(">hhI", rsrc,
                                                  tlist + refoff + 12 * j)
            off = data_off + (attr_off & 0xFFFFFF)
            n = struct.unpack_from(">I", rsrc, off)[0]
            out[rid] = rsrc[off + 4:off + 4 + n]
    return out


def cfrg_members(cfrg):
    """Yield (name, usage, where, offset, length) for PowerPC members."""
    count = struct.unpack_from(">H", cfrg, 30)[0]
    pos = 32
    for _ in range(count):
        (arch, _, _, _, _, _, _, _, usage, where, offset, length, _, _, _,
         size) = struct.unpack_from(">4sHBBIIIHBBIIIHHH", cfrg, pos)
        n = cfrg[pos + 42]
        name = cfrg[pos + 43:pos + 43 + n].decode("mac_roman")
        if arch == b"pwpc":
            yield name, usage, where, offset, length
        pos += size


def pef_exports(pef):
    """Return [(name, class)] exported by a PEF container."""
    if pef[:8] != b"Joy!peff":
        return []
    nsect = struct.unpack_from(">H", pef, 32)[0]
    for i in range(nsect):
        s = struct.unpack_from(">iIIIIIBBBB", pef, 40 + 28 * i)
        if s[6] == 4:
            loader = pef[s[5]:s[5] + s[4]]
            break
    else:
        return []
    (_, _, _, _, _, _, _, _, _, _, strings, hash_off, power,
     nexp) = struct.unpack_from(">iIiIiIIIIIIIII", loader, 0)
    keys = hash_off + 4 * (1 << power)
    syms = keys + 4 * nexp
    out = []
    for i in range(nexp):
        length = struct.unpack_from(">I", loader, keys + 4 * i)[0] >> 16
        word = struct.unpack_from(">I", loader, syms + 10 * i)[0]
        cls, off = word >> 24, word & 0xFFFFFF
        name = loader[strings + off:strings + off + length]
        out.append((name.decode("mac_roman"), cls & 0x0F))
    return out


def lzss(src, size):
    """Decompress the LZSS that the Mac OS ROM file uses (4 KiB window)."""
    ring = bytearray(b" " * 4096)
    r, out, i, flags = 4096 - 18, bytearray(), 0, 0
    while i < len(src) and len(out) < size:
        flags >>= 1
        if not flags & 0x100:
            flags = src[i] | 0xFF00
            i += 1
            if i >= len(src):
                break
        if flags & 1:
            c = src[i]
            i += 1
            out.append(c)
            ring[r] = c
            r = (r + 1) & 4095
        else:
            if i + 1 >= len(src):
                break
            a, b = src[i], src[i + 1]
            i += 2
            p, n = a | ((b & 0xF0) << 4), (b & 0x0F) + 3
            for k in range(n):
                c = ring[(p + k) & 4095]
                out.append(c)
                ring[r] = c
                r = (r + 1) & 4095
    return bytes(out)


def rom_image(data):
    """Find the ROM image in a NewWorld "Mac OS ROM" file's parcels."""
    base = data.find(b"prcl\x01")
    if base < 0:
        return None
    off = base + 20
    while 0 < off < len(data):
        nxt, ptype, hsize, _, nchild, csize = struct.unpack_from(
            ">I4sIIII", data, off)
        for c in range(nchild):
            e = off + hsize - nchild * csize + c * csize
            ctype, _, comp, size, _, clen, coff = struct.unpack_from(
                ">4sI4sIIII", data, e)
            if ptype == b"rom " and ctype == b"rom ":
                body = data[base + coff:base + coff + clen]
                return lzss(body, size) if comp == b"lzss" else body
        if not nxt:
            break
        off = base + nxt
    return None


def rom_libraries(rom):
    """Yield (name, pef) for the libraries in a ROM image.

    Each ROM resource has a header: data offset (4 bytes), type (4), id
    (2), attributes (1), name (Pascal string). Libraries are type 'nlib'
    or 'ncod' and their data is a PEF container."""
    for rtype in (b"nlib", b"ncod"):
        pos = rom.find(rtype)
        while pos >= 4:
            data_off = struct.unpack_from(">I", rom, pos - 4)[0]
            n = rom[pos + 7]
            if data_off + 8 <= len(rom) and \
                    rom[data_off:data_off + 8] == b"Joy!peff":
                name = rom[pos + 8:pos + 8 + n].decode("mac_roman")
                yield name, rom[data_off:]
            pos = rom.find(rtype, pos + 4)


def add(libs, name, exports, where):
    if not exports:
        return
    lib = libs.setdefault(name, {"func": set(), "data": set()})
    for sym, cls in exports:
        # 2 = TVector (a function); 0, 1, 3 = code, data, TOC
        (lib["func"] if cls == 2 else lib["data"]).add(sym)
    print(f"{where}: {name}: {len(exports)}", file=sys.stderr)


def read_fork(path):
    for p in (path + ":rsrc", os.path.join(path, "..namedfork", "rsrc")):
        if os.path.isfile(p):
            return open(p, "rb").read()
    return b""


def scan(path, libs):
    with open(path, "rb") as f:
        head = f.read(11)
    if head == b"<CHRP-BOOT>":
        rom = rom_image(open(path, "rb").read())
        for name, pef in rom_libraries(rom or b""):
            add(libs, name, pef_exports(pef), os.path.basename(path))
        return
    rsrc = read_fork(path)
    if not rsrc and os.path.getsize(path) in (2 << 20, 4 << 20):
        # A ROM image from an OldWorld Power Mac (7.6.1 to 8.1 keep
        # InterfaceLib there, not on disk).
        for name, pef in rom_libraries(open(path, "rb").read()):
            add(libs, name, pef_exports(pef), os.path.basename(path))
        return
    cfrgs = resources(rsrc, b"cfrg")
    if not cfrgs:
        return
    data = None
    for cfrg in cfrgs.values():
        for name, usage, where, offset, length in cfrg_members(cfrg):
            if usage != 0:                    # import libraries only
                continue
            if where == 1:                    # in the data fork
                if data is None:
                    data = open(path, "rb").read()
                pef = data[offset:offset + length] if length else \
                    data[offset:]
            elif where == 2:                  # in a resource ('nlib', ...)
                rtype = struct.pack(">I", offset)
                rid = struct.unpack(">h", struct.pack(">H", length & 0xFFFF))[0]
                pef = resources(rsrc, rtype).get(rid, b"")
            else:
                continue
            add(libs, name, pef_exports(pef), os.path.basename(path))


def safe_scan(path, libs):
    try:
        scan(path, libs)
    except (struct.error, IndexError, ValueError) as e:
        print(f"{path}: skipped, unreadable: {e}", file=sys.stderr)


def main():
    args = sys.argv[1:]
    keep_all = "--all" in args
    args = [a for a in args if a != "--all"]
    out, inputs = args[0], args[1:]
    libs = {}
    for p in inputs:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for f in sorted(files):
                    if not f.endswith(":rsrc"):
                        safe_scan(os.path.join(root, f), libs)
        else:
            safe_scan(p, libs)
    result = {n: {"func": sorted(v["func"]),
                  "data": sorted(v["data"] - v["func"]),
                  "weak": n in WEAK}
              for n, v in libs.items() if keep_all or n in PUBLIC}
    with open(out, "w", encoding="utf-8") as f:
        json.dump(result, f, separators=(",", ":"), sort_keys=True)
    print(f"{len(result)} libraries, "
          f"{sum(len(v['func']) + len(v['data']) for v in result.values())}"
          f" names", file=sys.stderr)


if __name__ == "__main__":
    main()
