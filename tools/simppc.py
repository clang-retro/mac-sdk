"""Run a PowerPC Mac application from peflink.py (or fatlink.py) on a
PowerPC CPU emulator, with the InterfaceLib functions it imports faked
by tools/sim68k.py's fake Memory Manager, File Manager and devices, and
ThreadsLib's cooperative threads by its FakeThreads, and Apple OpenGL
(OpenGLLibrary and OpenGLUtility) by tools/fakegl.py.

This checks what the 68k simulator cannot: the PEF container and its
relocations, the import glue and the TOC, and the C library's PowerPC
build. It is not a Mac: only the functions in FUNCS are known, and a
program that imports another one stops when it calls it.

    python3 simppc.py App.bin [--files] [--input TEXT] [--keys TEXT]
        [--steps N]

It prints each DrawString and DrawText, and with --files the files the
program left. --input fills "App Input" (the C library's stdin) and
--keys says what GetNextEvent types, as sim68k.py's do.

Needs the "unicorn" Python package.

Copyright (C) clang-retro contributors.
SPDX-License-Identifier: AGPL-3.0-only
"""
import argparse
import os
import struct
import sys
import time

from unicorn import Uc, UcError, UC_ARCH_PPC, UC_MODE_PPC32, \
    UC_MODE_BIG_ENDIAN, UC_HOOK_CODE, UC_HOOK_INTR
from unicorn import ppc_const

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sim68k import FakeNet, FakeOS, FakeScreen, FakeThreads, GMT_DELTA, GESTALT, \
    THREAD_STACK, key_events  # noqa: E402
from fakegl import FakeGL  # noqa: E402

MEM_SIZE = 32 << 20
CODE_AT, DATA_AT = 0x00400000, 0x00800000
SCRATCH = 0x00100000                # parameter blocks for the fakes
STUBS = 0x01E00000                  # one "blr" per import
TVECS = 0x01F00000                  # one transition vector per import
MAX_STUBS = 8192                    # imports and FindSymbol's functions
STACK_TOP = 0x01D00000
RETURN = STUBS - 4                  # where main returns to
THREAD_END = STUBS - 8              # where a thread's entry function returns to
BOOT = STUBS - 64                   # turns the FPU on and jumps to r12
BOOT_CODE = [0x7D6000A6,            # mfmsr r11
             0x616B2000,            # ori r11, r11, 0x2000 (MSR[FP])
             0x7D600124,            # mtmsr r11
             0x4C00012C,            # isync
             0x7D8903A6,            # mtctr r12
             0x4E800420]            # bctr

R = {f"r{i}": getattr(ppc_const, f"UC_PPC_REG_{i}") for i in range(32)}
PC, LR = ppc_const.UC_PPC_REG_PC, ppc_const.UC_PPC_REG_LR
# What a thread switch keeps: the integer and floating-point registers
# and the special registers that the code uses.
CONTEXT = list(R.values()) + [
    getattr(ppc_const, f"UC_PPC_REG_FPR{i}") for i in range(32)] + [
    ppc_const.UC_PPC_REG_CR, ppc_const.UC_PPC_REG_CTR, ppc_const.UC_PPC_REG_XER,
    ppc_const.UC_PPC_REG_FPSCR, LR, PC]


class PpcOS(FakeOS):
    """FakeOS with the 68k registers it reads kept here."""

    def __init__(self, uc):
        super().__init__(uc)
        self.regs = {"a0": 0, "d0": 0}
        self.top = 0x00A00000           # the heap, after the data
        self.heap_end = 0x01800000

    def a0(self):
        return self.regs["a0"]

    def d0(self):
        return self.regs["d0"]

    def set_a0(self, v):
        self.regs["a0"] = v & 0xFFFFFFFF

    def set_d0(self, v):
        self.regs["d0"] = v & 0xFFFFFFFF

    def call(self, kind, word, a0=0, d0=0):
        """Run a 68k trap's fake with A0 and D0; return (A0, D0 signed)."""
        self.regs = {"a0": a0 & 0xFFFFFFFF, "d0": d0 & 0xFFFFFFFF}
        if not getattr(self, kind)(word):
            raise SystemExit(f"simppc: no fake for trap {word:04X}")
        d = self.regs["d0"]
        return self.regs["a0"], d - (1 << 32) if d & 0x80000000 else d


def pef_of(path):
    """The PEF container in a MacBinary file's data fork (or a bare one)."""
    blob = open(path, "rb").read()
    if blob[:4] == b"Joy!":
        return blob
    dl = struct.unpack(">I", blob[83:87])[0]
    return blob[128:128 + dl]


def load(uc, pef):
    if pef[:12] != b"Joy!peffpwpc":
        sys.exit("simppc: not a PowerPC PEF container")
    n = struct.unpack_from(">H", pef, 32)[0]
    secs = [struct.unpack_from(">iIIIIIBBBB", pef, 40 + 28 * i)
            for i in range(n)]
    bases, loader = {}, None
    for i, (_, _, total, unpacked, packed, off, kind, _, _, _) in \
            enumerate(secs):
        body = pef[off:off + packed]
        if kind == 4:
            loader = body
        elif kind in (0, 1):
            at = CODE_AT if kind == 0 else DATA_AT
            uc.mem_write(at, body)
            bases[i] = at
        elif kind == 2:
            sys.exit("simppc: pattern-initialized data is not supported")
    if loader is None:
        sys.exit("simppc: no loader section")
    (main_sec, main_off, _, _, _, _, nlibs, nsyms, nrel, rel_off,
     str_off, _, _, _) = struct.unpack_from(">iIiIiIIIIIIIII", loader, 0)

    def string(o):
        end = loader.index(b"\0", str_off + o)
        return loader[str_off + o:end].decode("mac_roman")

    syms_at = 56 + 24 * nlibs
    imports = []
    for i in range(nsyms):
        w = struct.unpack_from(">I", loader, syms_at + 4 * i)[0]
        imports.append((string(w & 0xFFFFFF), (w >> 24) & 0x0F))
    # Each import is a transition vector whose code is a "blr" the hook
    # catches; data imports are not faked.
    for i, (name, cls) in enumerate(imports):
        uc.mem_write(STUBS + 4 * i, struct.pack(">I", 0x4E800020))
        uc.mem_write(TVECS + 8 * i, struct.pack(">II", STUBS + 4 * i, 0))
    rel_at = syms_at + 4 * nsyms
    for r in range(nrel):
        sec, _, count, first = struct.unpack_from(">HHII", loader,
                                                 rel_at + 12 * r)
        relocate(uc, loader[rel_off + 2 * first:rel_off + 2 * (first +
                                                               count)],
                 bases[sec], bases, imports)
    tv = bases[main_sec] + main_off
    code, toc = struct.unpack(">II", bytes(uc.mem_read(tv, 8)))
    return imports, code, toc


def relocate(uc, instrs, at, bases, imports):
    """The PEF relocation instructions that peflink.py writes."""
    code_base = bases.get(0, 0)
    data_base = bases.get(1, 0)
    words = struct.unpack(f">{len(instrs) // 2}H", instrs)

    def add(addr, v):
        old = struct.unpack(">I", bytes(uc.mem_read(addr, 4)))[0]
        uc.mem_write(addr, struct.pack(">I", (old + v) & 0xFFFFFFFF))

    i = 0
    while i < len(words):
        w = words[i]
        if w & 0xF000 == 0x8000:                # RelocIncrPosition
            at += (w & 0x0FFF) + 1
        elif w & 0xFC00 == 0xA000:              # RelocSetPosition
            i += 1
            at = bases[1] + (((w & 0x03FF) << 16) | words[i])
        elif w & 0xFE00 in (0x4000, 0x4200):    # RelocBySectC / D runs
            base = code_base if w & 0xFE00 == 0x4000 else data_base
            for _ in range((w & 0x01FF) + 1):
                add(at, base)
                at += 4
        elif w & 0xFE00 == 0x6000:              # RelocSmByImport
            add(at, TVECS + 8 * (w & 0x01FF))
            at += 4
        else:
            sys.exit(f"simppc: relocation instruction {w:04X} is not "
                     f"supported")
        i += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("app")
    ap.add_argument("--files", action="store_true",
                    help="print the files left at the end")
    ap.add_argument("--keys", metavar="TEXT", default="",
                    help="what GetNextEvent types (\\n is Return), then "
                    "Command-period")
    ap.add_argument("--input", metavar="TEXT",
                    help="the file \"App Input\" holds TEXT, with \\n as "
                    "the Mac's \\r")
    ap.add_argument("--steps", type=int, default=20_000_000,
                    help="the most instructions to run (default 20000000)")
    a = ap.parse_args()

    uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
    uc.mem_map(0, MEM_SIZE)
    os_ = PpcOS(uc)
    imports, entry, toc = load(uc, pef_of(a.app))
    screen = FakeScreen(os_)
    net = FakeNet(os_)
    GESTALT[b"qd  "] = 0x0230               # 32-bit Color QuickDraw 1.3

    blob = open(a.app, "rb").read(128)
    app = blob[2:2 + min(blob[1], 31)] if blob[:4] != b"Joy!" else b"App"
    uc.mem_write(0x910, bytes([len(app)]) + app)                # CurApName
    uc.mem_write(0x20C, struct.pack(">I", int(time.time()) + 2082844800 +
                                    GMT_DELTA))                 # Time
    if a.input is not None:
        os_.files[app.decode("mac_roman")[:25] + " Input"] = bytearray(
            a.input.replace("\\n", "\r").encode("mac_roman"))

    state = {"done": False}

    def arg(n):
        return uc.reg_read(R[f"r{3 + n}"]) & 0xFFFFFFFF

    def sarg(n):
        v = arg(n)
        return v - (1 << 32) if v & 0x80000000 else v

    def ret(v):
        uc.reg_write(R["r3"], v & 0xFFFFFFFF)

    def farg(n):
        """Floating-point argument n (in f1 to f13)."""
        bits = uc.reg_read(getattr(ppc_const, f"UC_PPC_REG_FPR{1 + n}"))
        return struct.unpack(">d", struct.pack(">Q", bits & (2**64 - 1)))[0]

    def pstring(addr):
        n = uc.mem_read(addr, 1)[0]
        return bytes(uc.mem_read(addr + 1, n)).decode("mac_roman")

    def pb(fields):
        """A zeroed parameter block at SCRATCH with fields set."""
        uc.mem_write(SCRATCH, bytes(128))
        for off, fmt, v in fields:
            uc.mem_write(SCRATCH + off, struct.pack(fmt, v))
        return SCRATCH

    def file_call(word, fields):
        return os_.call("file", word, pb(fields))[1]

    def memory(word):
        a0, err = os_.call("memory", word, arg(0), arg(1) if word == 0xA020
                           else arg(0))
        uc.mem_write(0x220, struct.pack(">h", err if err < 0 else 0))
        return a0, err

    def io(word):
        # FSRead / FSWrite (refNum, &count, buffer).
        count = struct.unpack(">i", bytes(uc.mem_read(arg(1), 4)))[0]
        err = file_call(word, [(24, ">h", sarg(0)), (32, ">I", arg(2)),
                               (36, ">i", count)])
        uc.mem_write(arg(1), bytes(uc.mem_read(SCRATCH + 40, 4)))
        return err

    def get_long(word, off):
        err = file_call(word, [(24, ">h", sarg(0))])
        uc.mem_write(arg(1), bytes(uc.mem_read(SCRATCH + off, 4)))
        return err

    def fs_open():
        err = file_call(0xA000, [(18, ">I", arg(0)), (22, ">h", sarg(1))])
        uc.mem_write(arg(2), bytes(uc.mem_read(SCRATCH + 24, 2)))
        return err

    def create():
        # Create(name, vRefNum, creator, type): the file and its type.
        err = file_call(0xA008, [(18, ">I", arg(0)), (22, ">h", sarg(1))])
        if err == 0:
            os_.types[os_.name(arg(0))] = struct.pack(">II", arg(3), arg(2))
        ret(err)

    def draw():
        print(f"DrawString({pstring(arg(0))!r})")

    keys = key_events(a.keys)

    def draw_text():
        text = bytes(uc.mem_read(arg(0) + sarg(1), sarg(2)))
        print(f"DrawText({text.decode('mac_roman')!r})")

    def set_rect():
        uc.mem_write(arg(0), struct.pack(">hhhh", sarg(2), sarg(1),
                                         sarg(4), sarg(3)))

    def next_event():
        ch, mods = next(keys)
        uc.mem_write(arg(1), struct.pack(">HIIIH", 3, ch, 0, 0, mods))
        ret(1)

    def exit_to_shell():
        state["done"] = True
        uc.emu_stop()

    def save_regs():
        return [uc.reg_read(r) for r in CONTEXT]

    def load_regs(regs):
        for r, v in zip(CONTEXT, regs):
            uc.reg_write(r, v)

    threads = FakeThreads(save_regs, load_regs)
    state["ticks"] = 1000

    def tick_count():
        state["ticks"] += 1
        ret(state["ticks"])

    def gestalt():
        sel = struct.pack(">I", arg(0))
        if arg(1):
            uc.mem_write(arg(1), struct.pack(">I", GESTALT.get(sel, 0)))
        ret(0 if sel in GESTALT else -5551)

    def new_thread():
        # NewThread(style, entry, param, stackSize, options, &result, &id):
        # entry is a transition vector (code, TOC).
        code, toc = struct.unpack(">II", bytes(uc.mem_read(arg(1), 8)))
        stack = arg(3) or THREAD_STACK
        base = os_.alloc(stack)
        if not base:
            ret(-108)
            return
        top = (base + stack - 64) & ~15
        uc.mem_write(top, bytes(64))            # the back chain ends here
        regs = save_regs()
        for reg, v in ((R["r1"], top), (R["r2"], toc), (R["r3"], arg(2)),
                       (LR, THREAD_END), (PC, code)):
            regs[CONTEXT.index(reg)] = v
        if arg(6):
            uc.mem_write(arg(6), struct.pack(">I", threads.new(regs)))
        ret(0)

    def yield_to(want):
        # The call returns noErr to its caller (LR) in this thread, later.
        ret(0)
        uc.reg_write(PC, uc.reg_read(LR))
        threads.switch(want)

    def dispose_thread():
        if arg(0) == threads.cur:
            thread_end()
        else:
            ret(0 if threads.dispose(arg(0)) else -618)

    def thread_end():
        if not threads.end():
            print("simppc: the last thread ended")
            uc.emu_stop()

    # The Toolbox calls that the console window makes and that do nothing
    # here.
    nothing = ["InitGraf", "InitFonts", "FlushEvents", "InitWindows",
               "InitMenus", "TEInit", "InitDialogs", "InitCursor", "SetPort",
               "TextFont", "TextSize", "EraseRect", "MoveTo", "SizeWindow",
               "MoveWindow", "ShowWindow", "DisposeWindow", "SystemTask",
               "BeginUpdate", "EndUpdate", "DragWindow", "SelectWindow",
               "SystemClick", "ScrollRect"]
    funcs = {n: (lambda: ret(0)) for n in nothing}
    funcs.update({
        "DrawText": draw_text,
        "SetRect": set_rect,
        "GetFontInfo": lambda: uc.mem_write(
            arg(0), struct.pack(">hhhh", 9, 2, 6, 0)),     # Monaco 9
        "GetNextEvent": next_event,
        "NewWindow": lambda: ret(0x30000),
        "NewRgn": lambda: ret(0x40000),
        "FindWindow": lambda: ret(0),
        "TrackGoAway": lambda: ret(0),
    })
    funcs.update({
        "DrawString": draw,
        "ExitToShell": exit_to_shell,
        "NewPtr": lambda: ret(memory(0xA11E)[0]),
        "NewPtrClear": lambda: ret(memory(0xA31E)[0]),
        "DisposePtr": lambda: memory(0xA01F),
        "SetPtrSize": lambda: memory(0xA020),
        "GetPtrSize": lambda: ret(memory(0xA021)[1]),
        "Create": create,
        "FSOpen": lambda: ret(fs_open()),
        "FSClose": lambda: ret(file_call(0xA001, [(24, ">h", sarg(0))])),
        "FSRead": lambda: ret(io(0xA002)),
        "FSWrite": lambda: ret(io(0xA003)),
        "GetEOF": lambda: ret(get_long(0xA011, 28)),
        "SetEOF": lambda: ret(file_call(0xA012, [(24, ">h", sarg(0)),
                                                 (28, ">i", sarg(1))])),
        "GetFPos": lambda: ret(get_long(0xA018, 46)),
        "SetFPos": lambda: ret(file_call(0xA044, [(24, ">h", sarg(0)),
                                                  (44, ">h", sarg(1)),
                                                  (46, ">i", sarg(2))])),
        "FlushVol": lambda: ret(0),
        "FSDelete": lambda: ret(file_call(0xA009, [(18, ">I", arg(0)),
                                                   (22, ">h", sarg(1))])),
        "Rename": lambda: ret(file_call(0xA00B, [(18, ">I", arg(0)),
                                                 (22, ">h", sarg(1)),
                                                 (28, ">I", arg(2))])),
        "PBOpenSync": lambda: ret(os_.call("file", 0xA000, arg(0))[1]),
        "PBFlushFileSync": lambda: ret(os_.call("file", 0xA045, arg(0))[1]),
        "PBHSetFLockSync": lambda: ret(os_.call("file", 0xA241, arg(0))[1]),
        "PBHRstFLockSync": lambda: ret(os_.call("file", 0xA242, arg(0))[1]),
        "PBHSetVolSync": lambda: ret(os_.call("file", 0xA215, arg(0))[1]),
        "PBGetCatInfoSync": lambda: ret(os_.call("hfs", 0xA260, arg(0),
                                                 9)[1]),
        "PBCatMoveSync": lambda: ret(os_.call("hfs", 0xA260, arg(0), 5)[1]),
        "PBDirCreateSync": lambda: ret(os_.call("hfs", 0xA260, arg(0),
                                                6)[1]),
        "PBGetFCBInfoSync": lambda: ret(os_.call("hfs", 0xA260, arg(0),
                                                 8)[1]),
        "PBSetCatInfoSync": lambda: ret(os_.call("hfs", 0xA260, arg(0),
                                                 10)[1]),
        "ReadLocation": lambda: os_.call("device", 0xA051, arg(0),
                                         0x000C00E4),
    })
    def get_port():
        uc.mem_write(arg(0), struct.pack(">I", screen.port))

    def wait_next_event():
        state["ticks"] += 1
        ret(screen.wait_event(arg(1), state["ticks"]))

    # What a program that shows its own pictures uses (FakeScreen).
    for n in ("HideWindow", "SetWTitle", "ForeColor", "BackColor",
              "GlobalToLocal", "LocalToGlobal", "HiliteMenu"):
        funcs[n] = lambda: ret(0)
    funcs.update({
        "NewWindow": lambda: ret(screen.new_window(arg(0), arg(1), sarg(7))),
        "NewCWindow": lambda: ret(screen.new_window(arg(0), arg(1), sarg(7))),
        "DisposeWindow": lambda: screen.dispose(arg(0)),
        "GetWRefCon": lambda: ret(screen.refcon(arg(0))),
        "SetPort": lambda: setattr(screen, "port", arg(0)),
        "GetPort": get_port,
        "GetMouse": lambda: uc.mem_write(arg(0), bytes(4)),
        "WaitNextEvent": wait_next_event,
        "MenuSelect": lambda: ret(0),
        "GetGrayRgn": lambda: ret(struct.unpack(
            ">I", bytes(uc.mem_read(0x9EE, 4)))[0]),
        "CopyBits": lambda: screen.copy_bits(arg(0), arg(1), arg(2), arg(3)),
        "NewHandle": lambda: ret(memory(0xA122)[0]),
        "NewHandleClear": lambda: ret(memory(0xA322)[0]),
        "DisposeHandle": lambda: memory(0xA023),
    })
    def open_driver():
        if pstring(arg(0)) != ".IPP":
            ret(-35)                            # nsvErr
            return
        uc.mem_write(arg(1), struct.pack(">h", FakeNet.IPP))
        ret(0)

    def control(is_async):
        pb = arg(0)
        if os_.r16(pb + 24) != FakeNet.IPP:
            ret(-28)                            # notOpenErr
            return
        ret(net.control(pb, is_async))

    funcs.update({
        "OpenDriver": open_driver,
        "PBControlSync": lambda: control(False),
        "PBControlAsync": lambda: control(True),
        "EventAvail": lambda: (uc.mem_write(arg(1), bytes(16)), ret(0)),
        "PPCInit": lambda: ret(0),
    })
    for i, n in enumerate(("Open", "Start", "Inform", "Accept", "Reject",
                           "Write", "Read", "End", "Close")):
        for kind in ("Sync", "Async"):
            funcs[f"PPC{n}{kind}"] = (
                lambda sel=i + 1: ret(net.ppc(sel, arg(0))))
    funcs.update({
        "TickCount": tick_count,
        "Gestalt": gestalt,
        "NewThread": new_thread,
        "YieldToAnyThread": lambda: yield_to(0),
        "YieldToThread": lambda: yield_to(arg(0)),
        "GetCurrentThread": lambda: (
            uc.mem_write(arg(0), struct.pack(">I", threads.cur)), ret(0)),
        "DisposeThread": dispose_thread,
        "ThreadBeginCritical": lambda: ret(0),
        "ThreadEndCritical": lambda: ret(0),
        # A routine descriptor here is the procedure's own transition
        # vector: the simulator runs only PowerPC code.
        "NewRoutineDescriptor": lambda: ret(arg(0)),
        "DisposeRoutineDescriptor": lambda: ret(0),
        # The Process Manager knows one process, and the Display Manager
        # sends no notices. FindFolder finds no folder.
        "GetCurrentProcess": lambda: (
            uc.mem_write(arg(0), struct.pack(">II", 0, 2)), ret(0)),
        "GetProcessInformation": lambda: ret(-600),   # procNotFound
        "DMRegisterNotifyProc": lambda: ret(0),
        "DMRemoveNotifyProc": lambda: ret(0),
        "FindFolder": lambda: ret(-43),               # fnfErr
    })

    # Apple OpenGL: each imported agl*, gl* and glu* function.
    gl = FakeGL(uc, arg, sarg, farg, ret, os_.alloc)

    def add_gl(name):
        if name.startswith(("agl", "gl")) and name not in funcs:
            funcs[name] = lambda n=name: gl.call(n)

    for name, *_ in imports:
        add_gl(name)

    # The Code Fragment Manager: GetSharedLibrary connects to a library
    # that the simulator fakes, and FindSymbol gives a function's
    # transition vector, adding a stub for a name the program does not
    # import.
    libraries = ["InterfaceLib", "ThreadsLib", "MathLib", "OpenGLLibrary",
                 "OpenGLUtility"]

    def get_shared_library():
        name = pstring(arg(0))
        if name not in libraries:
            ret(-2804)                          # cfragNoLibraryErr
            return
        if arg(3):
            uc.mem_write(arg(3), struct.pack(">I", 0x00C00000 +
                                             16 * libraries.index(name)))
        if arg(4):
            uc.mem_write(arg(4), struct.pack(">I", 0))
        ret(0)

    def find_symbol():
        name = pstring(arg(1))
        names = [n for n, _ in imports]
        if name in names:
            i = names.index(name)
        elif name in funcs or name.startswith(("agl", "gl")):
            i = len(imports)
            if i >= MAX_STUBS:
                ret(-2802)                      # cfragNoSymbolErr
                return
            imports.append((name, 2))
            uc.mem_write(STUBS + 4 * i, struct.pack(">I", 0x4E800020))
            uc.mem_write(TVECS + 8 * i, struct.pack(">II", STUBS + 4 * i, 0))
            add_gl(name)
        else:
            ret(-2802)                          # cfragNoSymbolErr
            return
        uc.mem_write(arg(2), struct.pack(">I", TVECS + 8 * i))
        if arg(3):
            uc.mem_write(arg(3), bytes([2]))    # kTVectorCFragSymbol
        ret(0)

    funcs["GetSharedLibrary"] = get_shared_library
    funcs["FindSymbol"] = find_symbol

    def on_code(uc, pc, size, _):
        if pc == THREAD_END:
            thread_end()
            return
        if pc == RETURN:
            state["done"] = True
            uc.emu_stop()
            return
        net.pump()
        i = (pc - STUBS) // 4
        name = imports[i][0]
        f = funcs.get(name)
        if f is None:
            print(f"simppc: {name} is not faked")
            uc.emu_stop()
            return
        f()

    def on_intr(uc, intno, _):
        print(f"simppc: CPU exception {intno} at "
              f"{uc.reg_read(PC) - CODE_AT:#x} in code")
        uc.emu_stop()

    uc.hook_add(UC_HOOK_CODE, on_code, begin=THREAD_END,
                end=STUBS + 4 * MAX_STUBS)
    uc.hook_add(UC_HOOK_INTR, on_intr)
    uc.mem_write(RETURN, struct.pack(">I", 0x4E800020))
    uc.mem_write(THREAD_END, struct.pack(">I", 0x4E800020))
    uc.mem_write(BOOT, struct.pack(f">{len(BOOT_CODE)}I", *BOOT_CODE))
    uc.reg_write(R["r12"], entry)
    uc.reg_write(R["r1"], STACK_TOP - 64)
    uc.reg_write(R["r2"], toc)
    uc.reg_write(LR, RETURN)
    try:
        uc.emu_start(BOOT, RETURN, count=a.steps)
    except UcError as e:
        sys.exit(f"simppc: {e} at code offset "
                 f"{uc.reg_read(PC) - CODE_AT:#x}")
    if uc.reg_read(PC) == RETURN:
        state["done"] = True            # main returned
    # The files, also when the program did not finish: they tell how
    # far it went.
    if a.files:
        for n, data in sorted(os_.files.items()):
            text = bytes(data).replace(b"\r", b"\n").decode("mac_roman")
            print(f"file {n!r}:\n{text}",
                  end="" if text.endswith("\n") else "\n")
    gl.summary()
    if not state["done"]:
        screen.summary()
        sys.exit("simppc: the program did not finish")
    if not screen.summary():
        sys.exit(1)
    print("simppc: the program ended")


if __name__ == "__main__":
    main()
