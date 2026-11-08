#!/usr/bin/env python3
"""Run a 68k Mac application from codelink.py on a 68k CPU emulator, with a
few Toolbox traps faked, and print the calls it makes.

This checks code generation (Pascal calls, trap words, stack balance and
relocation) without a Mac ROM. It is not a Mac: only the traps listed in
TRAPS are known, and they do nothing but log their arguments. With
--os, the Memory Manager's pointer calls and the File Manager's basic
calls (on files in memory, all in one folder) work too, enough for the C
library; --files then prints the files the program left.

    python3 sim68k.py Hello68.bin [--clicks N] [--cpu 68020] [--system6]
        [--os] [--files] [--exit-anywhere]

It runs as a 68000 unless --cpu says otherwise. --system6 makes
_FlushCodeCache unimplemented, as on System 6 on a Mac Plus or SE.

Needs the "unicorn" Python package.
"""
import argparse
import struct
import sys
import time

from unicorn import Uc, UcError, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, \
    UC_HOOK_INTR, UC_HOOK_CODE
from unicorn.m68k_const import UC_M68K_REG_A5, UC_M68K_REG_A7, \
    UC_M68K_REG_PC, UC_M68K_REG_D0, UC_M68K_REG_A0, \
    UC_CPU_M68K_M68000, UC_CPU_M68K_M68020, UC_CPU_M68K_M68030, \
    UC_CPU_M68K_M68040
CPUS = {"68000": UC_CPU_M68K_M68000, "68020": UC_CPU_M68K_M68020,
        "68030": UC_CPU_M68K_M68030, "68040": UC_CPU_M68K_M68040}
UNIMPL_ADDR = 0x00F0FFF0     # what the trap table holds for _Unimplemented

MEM_BASE, MEM_SIZE = 0, 4 << 20
CODE_AT = 0x20000          # where CODE 1 is "loaded"
A5_AT = 0x10000            # application globals pointer
STACK_TOP = MEM_SIZE - 0x100


def read_macbinary(path):
    mb = open(path, "rb").read()
    dl, rl = struct.unpack(">II", mb[83:91])
    roff = 128 + ((dl + 127) & ~127)
    return mb[roff:roff + rl]


def resources(rsrc, want):
    data_off, map_off = struct.unpack_from(">II", rsrc, 0)
    tlist = map_off + struct.unpack_from(">H", rsrc, map_off + 24)[0]
    out = {}
    for i in range(struct.unpack_from(">H", rsrc, tlist)[0] + 1):
        rtype, count, refoff = struct.unpack_from(">4sHH", rsrc,
                                                  tlist + 2 + 8 * i)
        if rtype != want:
            continue
        for j in range(count + 1):
            rid, _, ao = struct.unpack_from(">hhI", rsrc,
                                            tlist + refoff + 12 * j)
            off = data_off + (ao & 0xFFFFFF)
            n = struct.unpack_from(">I", rsrc, off)[0]
            out[rid] = rsrc[off + 4:off + 4 + n]
    return out


# trap word: (name, argument bytes, result bytes)
TRAPS = {
    0xA86E: ("InitGraf", 4, 0), 0xA8FE: ("InitFonts", 0, 0),
    0xA912: ("InitWindows", 0, 0), 0xA930: ("InitMenus", 0, 0),
    0xA9CC: ("TEInit", 0, 0), 0xA97B: ("InitDialogs", 4, 0),
    0xA850: ("InitCursor", 0, 0), 0xA913: ("NewWindow", 26, 4),
    0xAA45: ("NewCWindow", 26, 4),
    0xA873: ("SetPort", 4, 0), 0xA893: ("MoveTo", 4, 0),
    0xA884: ("DrawString", 4, 0), 0xA974: ("Button", 0, 2),
    0xA9F4: ("ExitToShell", 0, 0), 0xA032: ("FlushEvents", None, 0),
    0xA0BD: ("FlushCodeCache", None, 0),
    0xA746: ("GetToolTrapAddress", None, 0),
    0xA346: ("GetOSTrapAddress", None, 0),
}


HEAP_AT, HEAP_END = 0x200000, 0x3E0000
# Open, Close, Read, Write, Create, Delete, Rename, GetFileInfo,
# SetFileInfo, GetEOF, SetEOF, FlushVol, GetFPos, SetFPos.
FILE_TRAPS = {0xA000, 0xA001, 0xA002, 0xA003, 0xA008, 0xA009, 0xA00B,
              0xA00C, 0xA00D, 0xA011, 0xA012, 0xA013, 0xA018, 0xA044}


class FakeOS:
    """The Memory Manager's pointer calls and the File Manager's basic
    parameter block calls, on files kept in memory in one folder."""

    def __init__(self, uc):
        self.uc = uc
        self.files = {}         # name -> bytearray
        self.types = {}         # name -> (type, creator)
        self.open = {}          # refNum -> [name, mark]
        self.next_ref = 100
        self.blocks = {}        # address -> size
        self.top = HEAP_AT

    def rd(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def w16(self, a, v):
        self.uc.mem_write(a, struct.pack(">h", v))

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack(">I", v & 0xFFFFFFFF))

    def r16(self, a):
        return struct.unpack(">h", self.rd(a, 2))[0]

    def r32(self, a):
        return struct.unpack(">I", self.rd(a, 4))[0]

    def name(self, ptr):
        n = self.rd(ptr, 1)[0]
        return self.rd(ptr + 1, n).decode("mac_roman").lstrip(":")

    # Memory Manager: size in D0, pointer in A0, result in D0.
    def memory(self, word):
        uc = self.uc
        d0 = uc.reg_read(UC_M68K_REG_D0) & 0xFFFFFFFF
        a0 = uc.reg_read(UC_M68K_REG_A0) & 0xFFFFFFFF
        op = word & 0xF1FF
        err = 0
        if op == 0xA11E:                        # NewPtr
            size = (d0 + 3) & ~3
            if self.top + size > HEAP_END:
                uc.reg_write(UC_M68K_REG_A0, 0)
                err = -108
            else:
                p = self.top
                self.top += size or 4
                self.blocks[p] = d0
                uc.mem_write(p, bytes(d0))      # NewPtrClear or not
                uc.reg_write(UC_M68K_REG_A0, p)
        elif op == 0xA01F:                      # DisposePtr
            if self.blocks.pop(a0, None) is None:
                err = -111
        elif op == 0xA020:                      # SetPtrSize
            old = self.blocks.get(a0)
            if old is None:
                err = -111
            elif d0 <= old or a0 + ((old + 3) & ~3) == self.top:
                if d0 > old:
                    self.top = a0 + ((d0 + 3) & ~3)
                self.blocks[a0] = d0
            else:
                err = -108                      # cannot grow in place
        elif op == 0xA021:                      # GetPtrSize
            if a0 in self.blocks:
                uc.reg_write(UC_M68K_REG_D0, self.blocks[a0])
                return True
            err = -111
        else:
            return False
        uc.reg_write(UC_M68K_REG_D0, err)
        return True

    # File Manager: parameter block in A0, result in D0 and ioResult.
    def file(self, word):
        uc = self.uc
        pb = uc.reg_read(UC_M68K_REG_A0) & 0xFFFFFFFF
        op = word & 0xF9FF                      # without async and HFS bits
        if op not in FILE_TRAPS:
            return False
        ref = self.r16(pb + 24)
        err = 0
        f = self.open.get(ref)
        if op in (0xA001, 0xA002, 0xA003, 0xA011, 0xA012, 0xA018,
                  0xA044) and f is None:
            err = -51                           # rfNumErr
        elif op == 0xA008:                      # Create
            n = self.name(self.r32(pb + 18))
            if n in self.files:
                err = -48
            else:
                self.files[n] = bytearray()
        elif op == 0xA000:                      # Open
            n = self.name(self.r32(pb + 18))
            if n not in self.files:
                err = -43
            else:
                self.next_ref += 2
                self.open[self.next_ref] = [n, 0]
                self.w16(pb + 24, self.next_ref)
        elif op == 0xA001:                      # Close
            del self.open[ref]
        elif op in (0xA002, 0xA003):            # Read, Write
            data = self.files[f[0]]
            buf, want = self.r32(pb + 32), self.r32(pb + 36)
            mode, off = self.r16(pb + 44), struct.unpack(
                ">i", self.rd(pb + 46, 4))[0]
            if mode & 3 == 1:
                f[1] = off
            elif mode & 3 == 2:
                f[1] = len(data) + off
            elif mode & 3 == 3:
                f[1] += off
            if op == 0xA002:
                got = data[f[1]:f[1] + want]
                uc.mem_write(buf, bytes(got))
                n = len(got)
                if n < want:
                    err = -39                   # eofErr
            else:
                chunk = self.rd(buf, want)
                if f[1] > len(data):
                    data.extend(bytes(f[1] - len(data)))
                data[f[1]:f[1] + want] = chunk
                n = want
            f[1] += n
            self.w32(pb + 40, n)
            self.w32(pb + 46, f[1])
        elif op == 0xA011:                      # GetEOF
            self.w32(pb + 28, len(self.files[f[0]]))
        elif op == 0xA012:                      # SetEOF
            data, n = self.files[f[0]], self.r32(pb + 28)
            if n < len(data):
                del data[n:]
            else:
                data.extend(bytes(n - len(data)))
            f[1] = min(f[1], n)
        elif op == 0xA018:                      # GetFPos
            self.w32(pb + 46, f[1])
        elif op == 0xA044:                      # SetFPos
            mode = self.r16(pb + 44) & 3
            off = struct.unpack(">i", self.rd(pb + 46, 4))[0]
            base = {1: 0, 2: len(self.files[f[0]]), 3: f[1]}.get(mode, f[1])
            if base + off < 0:
                err = -40
            elif base + off > len(self.files[f[0]]):
                f[1] = len(self.files[f[0]])
                err = -39
            else:
                f[1] = base + off
            self.w32(pb + 46, f[1])
        elif op == 0xA009:                      # Delete
            n = self.name(self.r32(pb + 18))
            if n not in self.files:
                err = -43
            elif any(o[0] == n for o in self.open.values()):
                err = -47
            else:
                del self.files[n]
        elif op == 0xA00B:                      # Rename
            n = self.name(self.r32(pb + 18))
            new = self.name(self.r32(pb + 28))
            if n not in self.files:
                err = -43
            elif new in self.files:
                err = -48
            else:
                self.files[new] = self.files.pop(n)
        elif op == 0xA00C:                      # GetFileInfo
            n = self.name(self.r32(pb + 18))
            if n not in self.files:
                err = -43
            else:
                self.uc.mem_write(pb + 32, bytes(48))
        # SetFileInfo and FlushVol do nothing.
        self.w16(pb + 16, err)
        uc.reg_write(UC_M68K_REG_D0, err)
        return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("app")
    ap.add_argument("--clicks", type=int, default=3,
                    help="how many times Button() says no first")
    ap.add_argument("--cpu", choices=sorted(CPUS), default="68000")
    ap.add_argument("--system6", action="store_true",
                    help="no _FlushCodeCache, as on System 6 on a 68000")
    ap.add_argument("--os", action="store_true",
                    help="fake the Memory Manager and the File Manager")
    ap.add_argument("--files", action="store_true",
                    help="with --os, print the files left at the end")
    ap.add_argument("--exit-anywhere", action="store_true",
                    help="do not check the stack at ExitToShell (exit() "
                         "calls it from inside the program)")
    a = ap.parse_args()

    code = resources(read_macbinary(a.app), b"CODE")
    code0, code1 = code[0], code[1]
    above, below, jtsize, jtoff = struct.unpack_from(">IIII", code0, 0)
    entry = struct.unpack_from(">H", code0, 16)[0]

    uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
    uc.ctl_set_cpu_model(CPUS[a.cpu])
    uc.mem_map(MEM_BASE, MEM_SIZE)
    uc.mem_write(CODE_AT, code1)
    seg_start = CODE_AT + 4
    uc.reg_write(UC_M68K_REG_A5, A5_AT)
    uc.reg_write(UC_M68K_REG_A7, STACK_TOP)
    state = {"clicks": a.clicks, "done": False, "log": [], "port": None}
    fake = FakeOS(uc) if a.os else None
    # CurApName (0x910): the application's name, from the MacBinary header.
    mb = open(a.app, "rb").read(128)
    uc.mem_write(0x910, mb[1:2 + min(mb[1], 31)])
    # Time (0x20C): the clock, in seconds since 1904.
    uc.mem_write(0x20C, struct.pack(">I", int(time.time()) + 2082844800))

    def rd(addr, n):
        return bytes(uc.mem_read(addr, n))

    def pstring(addr):
        n = rd(addr, 1)[0]
        return rd(addr + 1, n).decode("mac_roman")

    def on_code(uc, pc, size, _):
        # Toolbox traps are A-line words: catch them before the CPU runs
        # them, fake them, and continue after the word.
        word = struct.unpack(">H", rd(pc, 2))[0]
        if word & 0xF000 != 0xA000:
            return
        if fake and word & 0x0800 == 0 and (fake.memory(word) or
                                            fake.file(word)):
            uc.reg_write(UC_M68K_REG_PC, pc + 2)
            return
        if word not in TRAPS:
            print(f"unknown trap {word:04X} at {pc - seg_start:#x}")
            uc.emu_stop()
            return
        name, nargs, nres = TRAPS[word]
        if name == "FlushCodeCache" and a.system6:
            print(f"_FlushCodeCache is unimplemented on System 6 "
                  f"(at {pc - seg_start:#x})")
            uc.emu_stop()
            return
        if name in ("GetToolTrapAddress", "GetOSTrapAddress"):
            num = uc.reg_read(UC_M68K_REG_D0) & 0x3FF
            missing = (name == "GetToolTrapAddress" and num == 0x9F) or \
                (name == "GetOSTrapAddress" and num == 0xBD and a.system6)
            addr = UNIMPL_ADDR if missing else 0x00F00000 + 4 * num
            uc.reg_write(UC_M68K_REG_A0, addr)
            print(f"{name}({num:#x}) = {addr:#x}")
            uc.reg_write(UC_M68K_REG_PC, pc + 2)
            return
        sp = uc.reg_read(UC_M68K_REG_A7)
        args = rd(sp, nargs) if nargs else b""
        desc = ""
        if name == "DrawString":
            desc = repr(pstring(struct.unpack(">I", args)[0]))
        elif name == "MoveTo":
            v, h = struct.unpack(">hh", args)
            desc = f"h={h} v={v}"
        elif name == "NewWindow" or name == "NewCWindow":
            (refcon, goaway, _, behind, procid, visible, _, title, rect,
             storage) = struct.unpack(">iBBIhBBIII", args)
            r = struct.unpack(">hhhh", rd(rect, 8))
            desc = (f"rect={r} title={pstring(title)!r} visible={visible} "
                    f"procID={procid} behind={behind:#x} goAway={goaway} "
                    f"refCon={refcon}")
        elif name in ("InitGraf", "SetPort", "InitDialogs"):
            desc = f"{struct.unpack('>I', args)[0]:#x}"
        elif name == "FlushEvents":
            desc = f"d0={uc.reg_read(UC_M68K_REG_D0):#x}"
        state["log"].append((name, desc))
        print(f"{name}({desc})")
        if nargs:
            sp += nargs
        if name == "NewWindow" or name == "NewCWindow":
            struct.pack_into(">I", buf := bytearray(4), 0, 0x30000)
            uc.mem_write(sp, bytes(buf))
        elif name == "Button":
            state["clicks"] -= 1
            uc.mem_write(sp, bytes([1 if state["clicks"] < 0 else 0, 0]))
        uc.reg_write(UC_M68K_REG_A7, sp)
        if name == "ExitToShell":
            state["done"] = True
            state["sp"] = sp
            uc.emu_stop()
            return
        uc.reg_write(UC_M68K_REG_PC, pc + 2)

    def on_intr(uc, intno, _):
        pc = uc.reg_read(UC_M68K_REG_PC)
        print(f"sim68k: CPU exception {intno} near segment offset "
              f"{pc - seg_start:#x}")
        uc.emu_stop()

    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.hook_add(UC_HOOK_INTR, on_intr)
    try:
        uc.emu_start(seg_start + entry, MEM_SIZE, count=2_000_000)
    except UcError as e:
        pc = uc.reg_read(UC_M68K_REG_PC)
        sys.exit(f"sim68k: {e} at {pc:#x} (segment offset "
                 f"{pc - seg_start:#x})")
    if not state["done"]:
        sys.exit("sim68k: the program did not reach ExitToShell")
    if fake and a.files:
        for n, data in sorted(fake.files.items()):
            text = bytes(data).replace(b"\r", b"\n").decode("mac_roman")
            print(f"file {n!r}:\n{text}", end="" if text.endswith("\n") else "\n")
    balance = state["sp"] - STACK_TOP
    print(f"sim68k: reached ExitToShell; stack {'balanced' if balance == 0 else f'off by {balance}'}")
    sys.exit(0 if balance == 0 or a.exit_anywhere else 1)


if __name__ == "__main__":
    main()
