#!/usr/bin/env python3
"""Run a 68k Mac application from codelink.py on a 68k CPU emulator, with a
few Toolbox traps faked, and print the calls it makes.

This checks code generation (Pascal calls, trap words, stack balance and
relocation) without a Mac ROM. It is not a Mac: only the traps listed in
TRAPS are known, and they do nothing but log their arguments.

    python3 sim68k.py Hello68.bin [--clicks N] [--cpu 68020] [--system6]

It runs as a 68000 unless --cpu says otherwise. --system6 makes
_FlushCodeCache unimplemented, as on System 6 on a Mac Plus or SE.

Needs the "unicorn" Python package.
"""
import argparse
import struct
import sys

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("app")
    ap.add_argument("--clicks", type=int, default=3,
                    help="how many times Button() says no first")
    ap.add_argument("--cpu", choices=sorted(CPUS), default="68000")
    ap.add_argument("--system6", action="store_true",
                    help="no _FlushCodeCache, as on System 6 on a 68000")
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
    balance = state["sp"] - STACK_TOP
    print(f"sim68k: reached ExitToShell; stack {'balanced' if balance == 0 else f'off by {balance}'}")
    sys.exit(0 if balance == 0 else 1)


if __name__ == "__main__":
    main()
