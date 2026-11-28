#!/usr/bin/env python3
"""Run a 68k Mac application from codelink.py on a 68k CPU emulator, with a
few Toolbox traps faked, and print the calls it makes.

This checks code generation (Pascal calls, trap words, stack balance and
relocation) without a Mac ROM. It is not a Mac: only the traps listed in
TRAPS are known, and they do nothing but log their arguments. With
--os, the Memory Manager's pointer calls and the File Manager's basic
calls (on files in memory, all in one folder) work too, enough for the C
library, and so do a pretend PowerBook's Power Manager (_PMgrOp) and the
Sound Driver's volume, write and KillIO calls, and ReadLocation, for a
Mac in UTC-8 (its clock, Time, is local time), the catalog calls of
HFS (folders, catalog information), Gestalt for the Thread Manager,
TempNewHandle (8 MB of temporary memory after the Mac's memory), and
the Thread Manager's cooperative threads (FakeThreads); --files then
prints the files the program left. An asynchronous write to the Sound Driver ends
at the program's next trap, which first runs its completion routine as
an interrupt would.

    python3 sim68k.py Hello68.bin [--clicks N] [--cpu 68020] [--system6]
        [--os] [--files] [--exit-anywhere]

It runs as a 68000 unless --cpu says otherwise. --system6 makes
_FlushCodeCache and _Microseconds unimplemented, as on System 6 on a Mac
Plus or SE. Otherwise _Microseconds gives a clock that moves forward
50 microseconds at each call.

Needs the "unicorn" Python package.
"""
import argparse
import struct
import sys
import time

from unicorn import Uc, UcError, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, \
    UC_HOOK_INTR, UC_HOOK_CODE
from unicorn import m68k_const
from unicorn.m68k_const import UC_M68K_REG_A5, UC_M68K_REG_A7, \
    UC_M68K_REG_PC, UC_M68K_REG_D0, UC_M68K_REG_A0, UC_M68K_REG_SR, \
    UC_CPU_M68K_M68000, UC_CPU_M68K_M68020, UC_CPU_M68K_M68030, \
    UC_CPU_M68K_M68040
CPUS = {"68000": UC_CPU_M68K_M68000, "68020": UC_CPU_M68K_M68020,
        "68030": UC_CPU_M68K_M68030, "68040": UC_CPU_M68K_M68040}
UNIMPL_ADDR = 0x00F0FFF0     # what the trap table holds for _Unimplemented

MEM_BASE, MEM_SIZE = 0, 4 << 20
CODE_AT = 0x20000          # where CODE 1 is "loaded"
A5_AT = 0x10000            # application globals pointer
STACK_TOP = MEM_SIZE - 0x100
IRQ_RETURN = MEM_SIZE - 0x10  # where a completion routine returns to
THREAD_END = MEM_SIZE - 0x20  # where a thread's entry function returns to
THREAD_STACK = 24 * 1024      # the Thread Manager's default stack size
GESTALT = {b"thds": 0x7,      # Thread Manager, specific match, ThreadsLib
           b"ppc ": 0x7,      # PPC Toolbox, real-time sessions
           b"os  ": 0x70}     # temporary memory: calls, real, tracked
# SR first: writing it can switch stacks.
REGS = [UC_M68K_REG_SR, UC_M68K_REG_PC] + [
    getattr(m68k_const, f"UC_M68K_REG_{r}{i}") for r in "DA" for i in range(8)]


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
    # What the C library's console window uses.
    0xA8A7: ("SetRect", 12, 0), 0xA887: ("TextFont", 2, 0),
    0xA88A: ("TextSize", 2, 0), 0xA88B: ("GetFontInfo", 4, 0),
    0xA8A3: ("EraseRect", 4, 0), 0xA885: ("DrawText", 8, 0),
    0xA91D: ("SizeWindow", 10, 0), 0xA91B: ("MoveWindow", 10, 0),
    0xA915: ("ShowWindow", 4, 0), 0xA914: ("DisposeWindow", 4, 0),
    0xA970: ("GetNextEvent", 6, 2), 0xA9B4: ("SystemTask", 0, 0),
    0xA922: ("BeginUpdate", 4, 0), 0xA923: ("EndUpdate", 4, 0),
    0xA92C: ("FindWindow", 8, 2), 0xA925: ("DragWindow", 12, 0),
    0xA91E: ("TrackGoAway", 8, 2), 0xA91F: ("SelectWindow", 4, 0),
    0xA9B3: ("SystemClick", 8, 0), 0xA8D8: ("NewRgn", 0, 4),
    0xA8EF: ("ScrollRect", 12, 0), 0xA975: ("TickCount", 0, 4),
    0xA823: ("FindFolder", 16, 2), 0xA99D: ("GetIndResource", 6, 4),
    # What a program that shows its own pictures uses (FakeScreen).
    0xA916: ("HideWindow", 4, 0), 0xA91A: ("SetWTitle", 8, 0),
    0xA917: ("GetWRefCon", 4, 4), 0xA862: ("ForeColor", 4, 0),
    0xA863: ("BackColor", 4, 0), 0xA874: ("GetPort", 4, 0),
    0xA871: ("GlobalToLocal", 4, 0), 0xA870: ("LocalToGlobal", 4, 0),
    0xA972: ("GetMouse", 4, 0), 0xA860: ("WaitNextEvent", 14, 2),
    0xA93D: ("MenuSelect", 4, 4), 0xA938: ("HiliteMenu", 2, 0),
    0xA8EC: ("CopyBits", 22, 0), 0xA971: ("EventAvail", 6, 2),
    # The Apple and File menus that a program puts in the menu bar.
    0xA931: ("NewMenu", 6, 4), 0xA933: ("AppendMenu", 8, 0),
    0xA935: ("InsertMenu", 6, 0), 0xA937: ("DrawMenuBar", 0, 0),
    0xA94D: ("AppendResMenu", 8, 0),
}
QUIET = {"SystemTask", "EraseRect", "MoveTo", "TickCount",   # not printed
         "HideWindow", "SetWTitle", "GetWRefCon", "ForeColor", "BackColor",
         "GetPort", "GlobalToLocal", "LocalToGlobal", "GetMouse",
         "WaitNextEvent", "MenuSelect", "HiliteMenu", "CopyBits",
         "EventAvail", "AppendMenu", "InsertMenu", "DrawMenuBar",
         "AppendResMenu"}


def key_events(text):
    """--keys: keyDown events for TEXT (\\n is Return), then Command-period
    for ever, which the console window reads as the end of input."""
    for c in text.replace("\\n", "\r"):
        yield ord(c) & 0xFF, 0
    while True:
        yield ord("."), 0x0100


GMT_DELTA = -8 * 3600     # the Mac's time zone, seconds east of UTC
HEAP_AT, HEAP_END = 0x200000, 0x3E0000
# Open, Close, Read, Write, Create, Delete, Rename, GetFileInfo,
# SetFileInfo, GetEOF, SetEOF, FlushVol, GetFPos, SetFPos.
FILE_TRAPS = {0xA000, 0xA001, 0xA002, 0xA003, 0xA008, 0xA009, 0xA00A, 0xA00B,
              0xA00C, 0xA00D, 0xA011, 0xA012, 0xA013, 0xA015, 0xA018,
              0xA041, 0xA042, 0xA044, 0xA045}


class FakeThreads:
    """The Thread Manager's cooperative threads. A thread that does not run
    is a saved set of registers. save() gives the registers of the CPU, and
    load() puts a set back. The application is thread 1."""

    def __init__(self, save, load):
        self.save, self.load = save, load
        self.cur = 1
        self.ready = {}         # thread ID -> registers, of threads not running
        self.next_id = 2

    def new(self, regs):
        """Adds a ready thread that starts with the registers regs, and
        gives its ID."""
        i = self.next_id
        self.next_id += 1
        self.ready[i] = regs
        return i

    def pick(self, want=0):
        """Gives the ID of the next thread to run: want if it is ready,
        otherwise the next ready ID after the current one, in a circle."""
        if want in self.ready:
            return want
        ids = sorted(self.ready)
        return next((i for i in ids if i > self.cur), ids[0])

    def switch(self, want=0):
        """Keeps the running thread's registers and runs another thread
        (YieldToThread). With no other thread ready, nothing changes."""
        if not self.ready:
            return
        nxt = self.pick(want)
        self.ready[self.cur] = self.save()
        self.cur = nxt
        self.load(self.ready.pop(nxt))

    def end(self):
        """The running thread ended (it returned, or DisposeThread of
        itself): runs the next ready thread. Gives False if none is
        left."""
        if not self.ready:
            return False
        nxt = self.pick()
        self.cur = nxt
        self.load(self.ready.pop(nxt))
        return True

    def dispose(self, i):
        """Deletes a thread that does not run. Gives False if there is no
        such thread."""
        return self.ready.pop(i, None) is not None


class FakeScreen:
    """A 640 by 480 screen with a 20-pixel menu bar, and the Window Manager
    and QuickDraw calls that a program makes to show its own pictures in
    plain windows: windows are records with a refCon and a portRect,
    CopyBits checks its source and destination and counts the pixels,
    WaitNextEvent gives null events and the mouse stays at 0, 0. Low
    memory gets MBarHeight (0xBAA) and GrayRgn (0x9EE). The other
    calls do nothing. summary() tells what the program did."""

    WIDTH, HEIGHT, MBAR = 640, 480, 20
    RECORD = 170                # sizeof(WindowRecord)

    def __init__(self, fake):
        self.fake, self.uc = fake, fake.uc
        self.port = 0
        self.windows = set()
        self.made = self.copies = self.pixels = self.waits = 0
        self.errors = []
        uc = self.uc
        uc.mem_write(0xBAA, struct.pack(">h", self.MBAR))
        rgn = fake.alloc(10)
        uc.mem_write(rgn, struct.pack(">hhhhh", 10, 0, 0, self.HEIGHT,
                                      self.WIDTH))
        handle = fake.alloc(4)
        uc.mem_write(handle, struct.pack(">I", rgn))
        uc.mem_write(0x9EE, struct.pack(">I", handle))

    def rect(self, addr):
        """Gives the Rect at addr as (top, left, bottom, right)."""
        return struct.unpack(">hhhh", bytes(self.uc.mem_read(addr, 8)))

    def new_window(self, storage, rect, refcon):
        """NewWindow and NewCWindow: gives the window record."""
        w = storage or self.fake.alloc(self.RECORD)
        if not w:
            return 0
        t, l, b, r = self.rect(rect)
        self.uc.mem_write(w, bytes(self.RECORD))
        self.uc.mem_write(w + 16, struct.pack(">hhhh", 0, 0, b - t, r - l))
        self.uc.mem_write(w + 152, struct.pack(">i", refcon))
        self.windows.add(w)
        self.made += 1
        return w

    def refcon(self, w):
        """GetWRefCon."""
        return struct.unpack(">i", bytes(self.uc.mem_read(w + 152, 4)))[0]

    def dispose(self, w):
        """DisposeWindow and CloseWindow."""
        self.windows.discard(w)

    def copy_bits(self, src, dst, src_rect, dst_rect):
        """CopyBits: the source rectangle must be in the source bitmap or
        PixMap, and the destination must be a window's port."""
        uc = self.uc
        row = struct.unpack(">h", bytes(uc.mem_read(src + 4, 2)))[0]
        bounds = self.rect(src + 6)
        sr, dr = self.rect(src_rect), self.rect(dst_rect)
        if not (bounds[0] <= sr[0] < sr[2] <= bounds[2] and
                bounds[1] <= sr[1] < sr[3] <= bounds[3]):
            self.errors.append(f"CopyBits: {sr} is not in {bounds}")
        depth = 32 if row & 0x8000 else 1
        width = (row & 0x3FFF) * 8 // depth
        if width < bounds[3] - bounds[1]:
            self.errors.append(f"CopyBits: rowBytes {row & 0x3FFF} is short "
                               f"for {bounds}")
        if dst - 2 not in self.windows:
            self.errors.append(f"CopyBits: {dst:#x} is not a window's port")
        self.copies += 1
        self.pixels += (sr[2] - sr[0]) * (sr[3] - sr[1])

    def wait_event(self, evp, ticks):
        """WaitNextEvent and GetNextEvent: a null event."""
        self.waits += 1
        self.uc.mem_write(evp, struct.pack(">HIIIH", 0, 0, ticks, 0, 0))
        return 0

    def summary(self):
        """Prints what the program did with the screen."""
        if self.made:
            print(f"screen: {self.made} windows made, {len(self.windows)} "
                  f"left, {self.copies} CopyBits of {self.pixels} pixels, "
                  f"{self.waits} WaitNextEvent")
        for e in self.errors[:10]:
            print(f"screen: {e}")
        return not self.errors


class FakeNet:
    """MacTCP's .IPP driver over the host's sockets, and the PPC Toolbox
    between the program's own ports. A call that cannot end at once
    stays in progress (ioResult 1) until pump() ends it; the simulators
    call pump() at each trap. Stream pointers are numbers that the
    program does not read."""

    IPP = -60                   # the driver's reference number
    LOOPBACK = 0x7F000001

    def __init__(self, fake):
        self.fake, self.uc = fake, fake.uc
        self.streams = {}       # stream pointer -> dict
        self.next_stream = 0x00F80000
        self.listeners = {}     # TCP port -> host listening socket
        self.pending = []       # (kind, pb, stream) of calls in progress
        self.ports = {}         # PPC port refNum -> name
        self.next_port = 1
        self.sessions = {}      # PPC session -> dict(peer, reads, writes, closed)
        self.next_sess = 1
        self.informs = []       # (pb, port) of PPCInform calls in progress
        self.starts = []        # (pb, port, name) of PPCStart calls in progress
        self.ppc_io = []        # PPCRead and PPCWrite calls in progress

    # -- memory --------------------------------------------------------
    def r8(self, a):
        return self.fake.rd(a, 1)[0]

    def done(self, pb, err):
        self.fake.w16(pb + 16, err)

    def new_stream(self, **kw):
        sp = self.next_stream
        self.next_stream += 16
        st = {"sock": None, "buf": bytearray(), "eof": False,
              "port": 0, "listening": False, "udp": None}
        st.update(kw)
        self.streams[sp] = st
        return sp

    # -- the .IPP driver -------------------------------------------------
    def control(self, pb, is_async):
        """A PBControl call of the .IPP driver. Gives the result now (1
        while it is in progress)."""
        import socket as so
        f = self.fake
        code = f.r16(pb + 26)
        sp = f.r32(pb + 28)
        st = self.streams.get(sp)
        err = 0
        self.done(pb, 1)
        if code == 15:                          # ipctlGetAddr
            f.w32(pb + 28, self.LOOPBACK)
            f.w32(pb + 32, 0xFF000000)
        elif code == 30:                        # TCPCreate
            f.w32(pb + 28, self.new_stream())
        elif code == 20:                        # UDPCreate
            u = so.socket(so.AF_INET, so.SOCK_DGRAM)
            port = f.r16(pb + 44) & 0xFFFF
            try:
                u.bind(("127.0.0.1", port))
            except OSError:
                err = -23017                    # duplicateSocket
            if not err:
                u.setblocking(False)
                sp = self.new_stream(udp=u, rcv=f.r32(pb + 32),
                                     rcvlen=f.r32(pb + 36))
                f.w32(pb + 28, sp)
                f.w16(pb + 44, u.getsockname()[1])
        elif st is None:
            err = -23010                        # invalidStreamPtr
        elif code == 31:                        # TCPPassiveOpen
            port = f.r16(pb + 46) & 0xFFFF
            lsock = self.listeners.get(port) if port else None
            if lsock is None:
                lsock = so.socket(so.AF_INET, so.SOCK_STREAM)
                lsock.setsockopt(so.SOL_SOCKET, so.SO_REUSEADDR, 1)
                try:
                    lsock.bind(("127.0.0.1", port))
                except OSError:                 # a port of another program
                    lsock.close()
                    lsock, err = None, -23017   # duplicateSocket
                if lsock:
                    lsock.listen(8)
                    lsock.setblocking(False)
                    port = lsock.getsockname()[1]
                    self.listeners[port] = lsock
            if lsock:
                st["port"], st["listening"] = port, True
                f.w16(pb + 46, port)
                self.pending.append(("accept", pb, sp))
        elif code == 32:                        # TCPActiveOpen
            host, port = f.r32(pb + 36), f.r16(pb + 40) & 0xFFFF
            c = so.socket(so.AF_INET, so.SOCK_STREAM)
            c.setblocking(False)
            c.connect_ex((so.inet_ntoa(struct.pack(">I", host)), port))
            st["sock"] = c
            self.pending.append(("connect", pb, sp))
        elif code == 34:                        # TCPSend
            if st["sock"] is None:
                err = -23008
            else:
                data, w = b"", f.r32(pb + 38)
                while True:
                    n = f.r16(w) & 0xFFFF
                    if n == 0:
                        break
                    data += f.rd(f.r32(w + 2), n)
                    w += 6
                st["sock"].setblocking(True)
                try:
                    st["sock"].sendall(data)
                except OSError:
                    err = -23012
                st["sock"].setblocking(False)
        elif code == 37:                        # TCPRcv
            self.pending.append(("rcv", pb, sp))
        elif code == 38:                        # TCPClose
            if st["sock"] is not None:
                try:
                    st["sock"].shutdown(so.SHUT_WR)
                except OSError:
                    pass
        elif code == 39:                        # TCPAbort
            self.abort(sp)
        elif code == 40:                        # TCPStatus
            if st["sock"] is None and not st["listening"]:
                err = -23008                    # connectionDoesntExist
            else:
                self.pull(st)
                state = 2 if st["sock"] is None else (14 if st["eof"] else 8)
                f.uc.mem_write(pb + 32, bytes(70))
                if st["sock"] is not None:
                    try:
                        lh, lp = st["sock"].getsockname()
                        rh, rp = st["sock"].getpeername()
                        f.w32(pb + 38, struct.unpack(">I", so.inet_aton(rh))[0])
                        f.w16(pb + 42, rp)
                        f.w32(pb + 44, struct.unpack(">I", so.inet_aton(lh))[0])
                        f.w16(pb + 48, lp)
                    except OSError:
                        state = 0
                else:
                    f.w16(pb + 48, st["port"])
                f.uc.mem_write(pb + 52, bytes([state]))
                f.w16(pb + 60, min(len(st["buf"]), 0xFFFF))
        elif code == 42:                        # TCPRelease
            self.abort(sp)
            gone = self.streams.pop(sp, None)
            # The last stream that waits on a port closes its listener, so
            # that a connection to the port is then refused.
            if gone and gone.get("listening"):
                port = gone["port"]
                if not any(o.get("listening") and o.get("port") == port
                           for o in self.streams.values()):
                    lsock = self.listeners.pop(port, None)
                    if lsock:
                        lsock.close()
        elif code == 21:                        # UDPRead
            self.pending.append(("udpread", pb, sp))
        elif code == 22:                        # UDPBfrReturn
            pass
        elif code == 23:                        # UDPWrite
            data, w = b"", f.r32(pb + 40)
            while True:
                n = f.r16(w) & 0xFFFF
                if n == 0:
                    break
                data += f.rd(f.r32(w + 2), n)
                w += 6
            host, port = f.r32(pb + 34), f.r16(pb + 38) & 0xFFFF
            st["udp"].sendto(data, (so.inet_ntoa(struct.pack(">I", host)), port))
        elif code == 24:                        # UDPRelease
            self.abort(sp)
            st["udp"].close()
            self.streams.pop(sp, None)
        else:
            err = -50
        if any(p[1] == pb for p in self.pending):
            self.pump()
            if not is_async:
                while f.r16(pb + 16) > 0:
                    self.pump(wait=True)
            return f.r16(pb + 16)
        self.done(pb, err)
        return err

    def abort(self, sp):
        """Ends a stream's calls in progress and its connection."""
        st = self.streams.get(sp)
        for p in [p for p in self.pending if p[2] == sp]:
            self.pending.remove(p)
            self.done(p[1], -23012)             # connectionTerminated
        if st and st["sock"] is not None:
            st["sock"].close()
            st["sock"] = None

    def pull(self, st):
        """Takes what the host socket has into the stream's buffer."""
        s = st["sock"]
        if s is None or st["eof"]:
            return
        while True:
            try:
                d = s.recv(65536)
            except BlockingIOError:
                return
            except OSError:
                st["eof"] = True
                return
            if not d:
                st["eof"] = True
                return
            st["buf"] += d

    def pump(self, wait=False):
        """Ends the calls in progress that can end."""
        import select
        import socket as so
        f = self.fake
        if wait:
            socks = [st["sock"] for st in self.streams.values()
                     if st["sock"] is not None]
            socks += [st["udp"] for st in self.streams.values() if st["udp"]]
            socks += list(self.listeners.values())
            select.select(socks, [], [], 0.05)
        for kind, pb, sp in list(self.pending):
            st = self.streams.get(sp)
            fin = None
            if kind == "accept":
                lsock = self.listeners.get(st["port"])
                try:
                    c, (h, p) = lsock.accept()
                except BlockingIOError:
                    continue
                c.setblocking(False)
                st["sock"], st["listening"] = c, False
                f.w32(pb + 36, struct.unpack(">I", so.inet_aton(h))[0])
                f.w16(pb + 40, p)
                f.w32(pb + 42, self.LOOPBACK)
                f.w16(pb + 46, c.getsockname()[1])
                fin = 0
            elif kind == "connect":
                r = select.select([], [st["sock"]], [], 0)[1]
                if not r:
                    continue
                e = st["sock"].getsockopt(so.SOL_SOCKET, so.SO_ERROR)
                if e:
                    st["sock"].close()
                    st["sock"] = None
                    fin = -23015                # openFailed
                else:
                    f.w32(pb + 42, self.LOOPBACK)
                    f.w16(pb + 46, st["sock"].getsockname()[1])
                    fin = 0
            elif kind == "rcv":
                self.pull(st)
                if st["buf"]:
                    n = min(len(st["buf"]), f.r16(pb + 40) & 0xFFFF)
                    f.uc.mem_write(f.r32(pb + 36), bytes(st["buf"][:n]))
                    del st["buf"][:n]
                    f.w16(pb + 40, n)
                    fin = 0
                elif st["eof"]:
                    fin = -23005                # connectionClosing
                else:
                    continue
            elif kind == "udpread":
                try:
                    d, (h, p) = st["udp"].recvfrom(65536)
                except BlockingIOError:
                    continue
                d = d[:st["rcvlen"]]
                f.uc.mem_write(st["rcv"], d)
                f.w32(pb + 34, struct.unpack(">I", so.inet_aton(h))[0])
                f.w16(pb + 38, p)
                f.w32(pb + 40, st["rcv"])
                f.w16(pb + 44, len(d))
                fin = 0
            self.pending.remove((kind, pb, sp))
            self.done(pb, fin)
        self.ppc_pump()

    # -- the PPC Toolbox -------------------------------------------------
    def port_name(self, rec):
        """The name of a PPCPortRec (its name and type strings)."""
        f = self.fake
        name = f.rd(rec + 3, self.r8(rec + 2))
        return name

    def ppc(self, sel, pb):
        """A PPC Toolbox call (selector sel). Gives the result now."""
        f = self.fake
        self.done(pb, 1)
        err = 0
        if sel == 0:                            # PPCInit
            return 0
        if sel == 1:                            # PPCOpen
            name = self.port_name(f.r32(pb + 46))
            if name in self.ports.values():
                err = -910                      # portNameExistsErr
            else:
                ref = self.next_port
                self.next_port += 1
                self.ports[ref] = name
                f.w16(pb + 38, ref)
        elif sel == 2:                          # PPCStart
            name = self.port_name(f.r32(pb + 46))
            if name not in self.ports.values():
                err = -903                      # noPortErr
            else:
                self.starts.append((pb, f.r16(pb + 38), name))
        elif sel == 3:                          # PPCInform
            self.informs.append((pb, f.r16(pb + 38)))
        elif sel in (6, 7):                     # PPCWrite, PPCRead
            sess = f.r32(pb + 40)
            if sess not in self.sessions or self.sessions[sess]["closed"]:
                err = -917                      # sessClosedErr
            else:
                self.ppc_io.append((sel, pb, sess))
        elif sel == 8:                          # PPCEnd
            self.end_session(f.r32(pb + 40))
        elif sel == 9:                          # PPCClose
            ref = f.r16(pb + 38)
            self.ports.pop(ref, None)
            for p in [p for p in self.informs if p[1] == ref]:
                self.informs.remove(p)
                self.done(p[0], -916)           # portClosedErr
            for s, d in list(self.sessions.items()):
                if d["port"] == ref:
                    self.end_session(s)
        else:
            err = -50
        if sel in (2, 3, 6, 7) and not err:
            # Start, Inform, Write and Read wait for the other end.
            self.ppc_pump()
            return 0
        self.done(pb, err)
        return err

    def end_session(self, sess):
        """Ends a session and its other end."""
        d = self.sessions.get(sess)
        if not d:
            return
        for s in (sess, d["peer"]):
            if s in self.sessions:
                self.sessions[s]["closed"] = True
        for p in [p for p in self.ppc_io if p[2] in (sess, d["peer"])]:
            self.ppc_io.remove(p)
            self.done(p[1], -917)

    def ppc_pump(self):
        """Ends the PPC calls that can end: a start meets an inform, and a
        write meets a read of the other end."""
        f = self.fake
        for start in list(self.starts):
            pb, port, name = start
            target = next((r for r, n in self.ports.items() if n == name),
                          None)
            inf = next((i for i in self.informs if i[1] == target), None)
            if inf is None:
                continue
            a, b = self.next_sess, self.next_sess + 1
            self.next_sess += 2
            self.sessions[a] = {"peer": b, "closed": False, "port": port}
            self.sessions[b] = {"peer": a, "closed": False, "port": target}
            self.starts.remove(start)
            self.informs.remove(inf)
            f.w32(pb + 40, a)
            ipb = inf[0]
            f.w32(ipb + 40, b)
            rec = f.r32(ipb + 46)
            if rec:
                n = self.ports.get(port, b"")
                f.uc.mem_write(rec + 2, bytes([len(n)]) + n)
            self.done(pb, 0)
            self.done(ipb, 0)
        for w in [p for p in self.ppc_io if p[0] == 6]:
            _, wpb, sess = w
            peer = self.sessions[sess]["peer"]
            r = next((p for p in self.ppc_io if p[0] == 7 and p[2] == peer),
                     None)
            if r is None:
                continue
            rpb = r[1]
            total, done = f.r32(wpb + 44), f.r32(wpb + 48)
            room = f.r32(rpb + 44)
            n = min(total - done, room)
            f.uc.mem_write(f.r32(rpb + 52),
                           f.rd(f.r32(wpb + 52) + done, n))
            f.w32(rpb + 48, n)
            f.uc.mem_write(rpb + 56, bytes([1 if done + n < total else 0]))
            f.w32(rpb + 66, f.r32(wpb + 66))    # blockType
            f.w32(wpb + 48, done + n)
            self.ppc_io.remove(r)
            self.done(rpb, 0)
            if done + n >= total:
                self.ppc_io.remove(w)
                self.done(wpb, 0)


class FakeOS:
    """The Memory Manager's pointer calls and the File Manager's basic
    parameter block calls, on files kept in memory in one folder."""

    def __init__(self, uc):
        self.uc = uc
        self.files = {}         # name -> bytearray
        self.forks = {}         # name -> resource fork (bytearray)
        self.types = {}         # name -> (type, creator)
        self.open = {}          # refNum -> [name, mark]
        self.next_ref = 100
        self.blocks = {}        # address -> size
        self.top = HEAP_AT
        self.heap_end = HEAP_END
        self.wakeup = (bytes(4), 0)  # wakeup time, enabled
        self.sound = []         # asynchronous Sound Driver writes, oldest first
        self.dirs = {"": 2}     # folder name -> directory ID

    # The registers the calls use: A0 and D0 here; tools/simppc.py, which
    # calls these for InterfaceLib's functions, keeps its own.
    def a0(self):
        return self.uc.reg_read(UC_M68K_REG_A0) & 0xFFFFFFFF

    def d0(self):
        return self.uc.reg_read(UC_M68K_REG_D0) & 0xFFFFFFFF

    def set_a0(self, v):
        self.uc.reg_write(UC_M68K_REG_A0, v & 0xFFFFFFFF)

    def set_d0(self, v):
        self.uc.reg_write(UC_M68K_REG_D0, v & 0xFFFFFFFF)

    def rd(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def w16(self, a, v):
        self.uc.mem_write(a, struct.pack(">H", v & 0xFFFF))

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack(">I", v & 0xFFFFFFFF))

    def r16(self, a):
        return struct.unpack(">h", self.rd(a, 2))[0]

    def r32(self, a):
        return struct.unpack(">I", self.rd(a, 4))[0]

    def name(self, ptr):
        n = self.rd(ptr, 1)[0]
        return self.rd(ptr + 1, n).decode("mac_roman").lstrip(":")

    # Temporary memory (TempNewHandle): a region of its own, outside the
    # heap, as the Process Manager's free memory is outside the
    # partition. map_temp maps it.
    TEMP_SIZE = 8 << 20

    def map_temp(self, at):
        """Map the temporary memory region at address at."""
        self.uc.mem_map(at, self.TEMP_SIZE)
        self.temp_top, self.temp_end = at, at + self.TEMP_SIZE

    def temp_new_handle(self, size):
        """TempNewHandle: give a handle to size bytes of temporary memory,
        and the result code."""
        size = (max(size, 4) + 3) & ~3
        end = getattr(self, "temp_end", 0)
        if not end or self.temp_top + size > end:
            return 0, -108                      # memFullErr
        h = self.alloc(4)
        if not h:
            return 0, -108
        p = self.temp_top
        self.temp_top += size
        self.uc.mem_write(p, bytes(size))
        self.w32(h, p)
        return h, 0

    def alloc(self, size):
        """Takes size bytes from the heap, as NewPtr does, for a thread's
        stack. Gives the address, or 0 when the heap is full."""
        size = (size + 3) & ~3
        if self.top + size > self.heap_end:
            return 0
        p = self.top
        self.top += size
        self.blocks[p] = size
        return p

    # Memory Manager: size in D0, pointer in A0, result in D0.
    def memory(self, word):
        uc = self.uc
        d0 = self.d0()
        a0 = self.a0()
        op = word & 0xF1FF
        err = 0
        if op == 0xA11E:                        # NewPtr
            size = (d0 + 3) & ~3
            if self.top + size > self.heap_end:
                self.set_a0(0)
                err = -108
            else:
                p = self.top
                self.top += size or 4
                self.blocks[p] = d0
                uc.mem_write(p, bytes(d0))      # NewPtrClear or not
                self.set_a0(p)
        elif op == 0xA122:                      # NewHandle, NewHandleClear
            p = self.alloc(max(d0, 4))
            h = self.alloc(4) if p else 0
            if not h:
                self.set_a0(0)
                err = -108
            else:
                uc.mem_write(p, bytes(d0))
                uc.mem_write(h, struct.pack(">I", p))
                self.set_a0(h)
        elif op in (0xA023, 0xA029, 0xA02A):    # DisposeHandle, HLock, HUnlock
            pass
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
                self.set_d0(self.blocks[a0])
                return True
            err = -111
        else:
            return False
        self.set_d0(err)
        return True

    # The Power Manager (_PMgrOp, a pmCommandRec in A0) with a battery,
    # a modem and a wakeup timer, and the Sound Driver (refNum -4): its
    # volume control call (csCode 2), which sets SdVolume, writes, which
    # end at once or (asynchronous) at the next trap, and KillIO, which
    # cancels the asynchronous writes without calling their completion
    # routines, as the Device Manager of the time did.
    def device(self, word):
        uc = self.uc
        pb = self.a0()
        if word == 0xA085:
            cmd, n = self.r16(pb), self.r16(pb + 2)
            send, recv = self.r32(pb + 4), self.r32(pb + 8)
            data = self.rd(send, n) if n else b""
            reply = {0x68: b"\x05\xC8", 0x58: b"\x0A"}.get(cmd)
            if cmd == 0x80:                     # set the wakeup time
                self.wakeup = (data, 1)
            elif cmd == 0x82:                   # disable it
                self.wakeup = (self.wakeup[0], 0)
            elif cmd == 0x88:                   # read it
                reply = self.wakeup[0] + bytes([self.wakeup[1]])
            if reply:
                uc.mem_write(recv, reply)
            self.set_d0(0)
            return True
        if word & 0xF9FF == 0xA004 and self.r16(pb + 24) == -4:
            if self.r16(pb + 26) == 2:
                uc.mem_write(0x260, bytes([self.r16(pb + 28) & 7]))
            self.set_d0(0)
            self.w16(pb + 16, 0)
            return True
        if word & 0xF9FF == 0xA003 and self.r16(pb + 24) == -4:
            self.w32(pb + 40, self.r32(pb + 36))    # ioActCount
            if word & 0x0400:
                self.w16(pb + 16, 1)
                self.sound.append(pb)
            else:
                self.w16(pb + 16, 0)
            self.set_d0(0)
            return True
        if word & 0xF9FF == 0xA006 and self.r16(pb + 24) == -4:
            for p in self.sound:
                self.w16(p + 16, -27)           # abortErr
            self.sound.clear()
            self.w16(pb + 16, 0)
            self.set_d0(0)
            return True
        if word == 0xA051 and self.d0() == 0x000C00E4:
            # ReadLocation: latitude, longitude, gmtDelta (24 bits).
            uc.mem_write(pb, bytes(8) + struct.pack(">I", GMT_DELTA & 0xFFFFFF))
            self.set_d0(0)
            return True
        return False

    # The oldest asynchronous Sound Driver write ends: its ioResult, and
    # its completion routine (or None).
    def sound_done(self):
        pb = self.sound.pop(0)
        self.w16(pb + 16, 0)
        return pb, self.r32(pb + 12) or None

    # The HFS calls (_HFSDispatch): CatMove (5), DirCreate (6),
    # GetFCBInfo (8), GetCatInfo (9), by name, by index in a folder or the
    # folder itself, and SetCatInfo (10), which sets the modification
    # date. Folders are the "a:" prefixes of the names; the current folder
    # is the volume's root, ID 2, on the volume "Sim".
    def folder_of(self, n):
        """Gives the folder name that holds the item n ("" for the root)."""
        return n.rsplit(":", 1)[0] if ":" in n else ""

    def is_dir(self, n):
        """Tells if n is a folder: the root, a made folder or a name prefix."""
        return n == "" or n in self.dirs or any(
            f.startswith(n + ":") for f in self.files)

    def dir_id(self, n):
        """Gives the folder ID of n, and assigns a new ID on first use."""
        if n not in self.dirs:
            self.dirs[n] = 100 + len(self.dirs)
        return self.dirs[n]

    def file_id(self, n):
        """Gives the file ID of n, and assigns a new ID on first use."""
        ids = self.__dict__.setdefault("file_ids", {})
        if n not in ids:
            ids[n] = 1000 + len(ids)
        return ids[n]

    def children(self, folder):
        """Gives the sorted names of the items directly in a folder."""
        items = set()
        pre = folder + ":" if folder else ""
        for n in list(self.files) + [d for d in self.dirs if d]:
            if n.startswith(pre) and n != folder:
                items.add(pre + n[len(pre):].split(":", 1)[0])
        return sorted(items, key=str.lower)

    def by_id(self, i):
        """Gives the folder name for a folder ID, or None if unknown."""
        if i in (0, 2):
            return ""
        return {v: k for k, v in self.dirs.items()}.get(i)

    def hfs(self, word):
        uc = self.uc
        if word & 0xF9FF != 0xA060:
            return False
        pb = self.a0()
        sel = self.d0() & 0xFFFF
        if sel not in (5, 6, 8, 9, 10):
            return False
        err = 0
        dates = self.__dict__.setdefault("dates", {})

        def named(dir_off):
            # The item a call names: its name, relative to the folder whose
            # ID is at dir_off (0: the current folder).
            ptr = self.r32(pb + 18)
            n = self.name(ptr).rstrip(":") if ptr else ""
            base = self.by_id(self.r32(pb + dir_off))
            if base is None:
                return None
            return (base + ":" + n if base and n else base or n)

        if sel == 9:                            # GetCatInfo
            ptr, index = self.r32(pb + 18), self.r16(pb + 28)
            if index < 0:
                n = self.by_id(self.r32(pb + 48))
            elif index > 0:
                folder = self.by_id(self.r32(pb + 48))
                kids = self.children(folder) if folder is not None else []
                n = kids[index - 1] if index <= len(kids) else None
            else:
                n = named(48)
            if n is None or (n not in self.files and not self.is_dir(n)):
                err = -43 if n is not None or index > 0 else -120
            else:
                leaf = n.rsplit(":", 1)[-1] if n else "Sim"
                if ptr and index != 0:
                    raw = leaf.encode("mac_roman")
                    uc.mem_write(ptr, bytes([len(raw)]) + raw)
                parent = self.folder_of(n)
                uc.mem_write(pb + 32, bytes(76))
                date = dates.get(n) or self.r32(0x20C) or 0xB0000000
                self.w32(pb + 72, date)
                self.w32(pb + 76, date)
                self.w16(pb + 22, -1)
                if n in self.files:
                    locked = n in self.__dict__.get("locked", ())
                    uc.mem_write(pb + 30, b"\1" if locked else b"\0")
                    uc.mem_write(pb + 32, self.types.get(n, bytes(8)))
                    self.w32(pb + 48, self.file_id(n))
                    self.w32(pb + 54, len(self.files[n]))
                    self.w32(pb + 58, (len(self.files[n]) + 511) & ~511)
                    self.w32(pb + 100, self.dir_id(parent) if parent else 2)
                else:
                    uc.mem_write(pb + 30, b"\x10")
                    self.w32(pb + 48, self.dir_id(n) if n else 2)
                    self.w16(pb + 52, len(self.children(n)))
                    self.w32(pb + 100, (self.dir_id(parent) if parent else 2)
                             if n else 1)
        elif sel == 6:                          # DirCreate
            n = named(48)
            if n is None:
                err = -120
            elif n in self.files or n in self.dirs:
                err = -48
            else:
                self.w32(pb + 48, self.dir_id(n))
        elif sel == 10:                         # SetCatInfo
            n = named(48)
            if n is None or (n not in self.files and not self.is_dir(n)):
                err = -43
            else:
                dates[n] = self.r32(pb + 76)
        elif sel == 8:                          # GetFCBInfo
            f = self.open.get(self.r16(pb + 24))
            if f is None:
                err = -51
            else:
                ptr = self.r32(pb + 18)
                if ptr:
                    raw = f[0].rsplit(":", 1)[-1].encode("mac_roman")
                    uc.mem_write(ptr, bytes([len(raw)]) + raw)
                parent = self.folder_of(f[0])
                self.w32(pb + 32, self.file_id(f[0]))
                self.w32(pb + 40, len(self.fork_of(f)))
                self.w32(pb + 48, f[1])
                self.w16(pb + 52, -1)
                self.w32(pb + 58, self.dir_id(parent) if parent else 2)
        else:                                   # CatMove
            n = self.name(self.r32(pb + 18))
            dest = self.by_id(self.r32(pb + 36))
            if self.r32(pb + 28):
                err = -50                       # ioNewName: not faked
            elif n not in self.files:
                err = -43
            elif dest is None:
                err = -120                      # dirNFErr
            else:
                new = (dest + ":" if dest else "") + n.rsplit(":", 1)[-1]
                if new in self.files:
                    err = -48
                else:
                    self.files[new] = self.files.pop(n)
                    if n in self.forks:
                        self.forks[new] = self.forks.pop(n)
        self.w16(pb + 16, err)
        self.set_d0(err)
        return True

    def fork_of(self, f):
        """The bytes of the fork that the open file f ([name, mark,
        resource fork]) reads and writes."""
        return self.forks[f[0]] if f[2] else self.files[f[0]]

    # File Manager: parameter block in A0, result in D0 and ioResult.
    def file(self, word):
        uc = self.uc
        pb = self.a0()
        op = word & 0xF9FF                      # without async and HFS bits
        if op not in FILE_TRAPS:
            return False
        ref = self.r16(pb + 24)
        err = 0
        f = self.open.get(ref)
        if op in (0xA001, 0xA002, 0xA003, 0xA011, 0xA012, 0xA018,
                  0xA044, 0xA045) and f is None:
            err = -51                           # rfNumErr
        elif op == 0xA008:                      # Create
            n = self.name(self.r32(pb + 18))
            if n in self.files:
                err = -48
            else:
                self.files[n] = bytearray()
        elif op in (0xA000, 0xA00A):            # Open, OpenRF
            n = self.name(self.r32(pb + 18))
            if n not in self.files:
                err = -43
            else:
                rsrc = op == 0xA00A
                if rsrc:
                    self.forks.setdefault(n, bytearray())
                self.next_ref += 2
                self.open[self.next_ref] = [n, 0, rsrc]
                self.w16(pb + 24, self.next_ref)
        elif op == 0xA001:                      # Close
            del self.open[ref]
        elif op in (0xA002, 0xA003):            # Read, Write
            data = self.fork_of(f)
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
            self.w32(pb + 28, len(self.fork_of(f)))
        elif op == 0xA012:                      # SetEOF
            data, n = self.fork_of(f), self.r32(pb + 28)
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
            base = {1: 0, 2: len(self.fork_of(f)), 3: f[1]}.get(mode, f[1])
            if base + off < 0:
                err = -40
            elif base + off > len(self.fork_of(f)):
                f[1] = len(self.fork_of(f))
                err = -39
            else:
                f[1] = base + off
            self.w32(pb + 46, f[1])
        elif op == 0xA009:                      # Delete
            n = self.name(self.r32(pb + 18)).rstrip(":")
            if n not in self.files and n in self.dirs:
                if self.children(n):
                    err = -47                   # fBsyErr: not empty
                else:
                    del self.dirs[n]
            elif n not in self.files:
                err = -43
            elif any(o[0] == n for o in self.open.values()):
                err = -47
            else:
                del self.files[n]
                self.forks.pop(n, None)
        elif op == 0xA00B:                      # Rename
            n = self.name(self.r32(pb + 18))
            new = self.name(self.r32(pb + 28))
            if n not in self.files:
                err = -43
            elif new in self.files:
                err = -48
            else:
                self.files[new] = self.files.pop(n)
                if n in self.forks:
                    self.forks[new] = self.forks.pop(n)
        elif op == 0xA00C:                      # GetFileInfo
            n = self.name(self.r32(pb + 18))
            if n not in self.files:
                err = -43
            else:
                self.uc.mem_write(pb + 32, bytes(48))
                self.uc.mem_write(pb + 32, self.types.get(n, bytes(8)))
        elif op == 0xA00D:                      # SetFileInfo: the type
            n = self.name(self.r32(pb + 18))
            if n not in self.files:
                err = -43
            else:
                self.types[n] = self.rd(pb + 32, 8)
        elif op in (0xA041, 0xA042):            # SetFilLock, RstFilLock
            n = self.name(self.r32(pb + 18))
            locked = self.__dict__.setdefault("locked", set())
            if n not in self.files:
                err = -43
            elif op == 0xA041:
                locked.add(n)
            else:
                locked.discard(n)
        # FlushVol, SetVol and FlushFile do nothing.
        self.w16(pb + 16, err)
        self.set_d0(err)
        return True


def main():
    global MEM_SIZE, STACK_TOP, IRQ_RETURN, THREAD_END, HEAP_AT, HEAP_END
    ap = argparse.ArgumentParser()
    ap.add_argument("app")
    ap.add_argument("--clicks", type=int, default=3,
                    help="how many times Button() says no first")
    ap.add_argument("--cpu", choices=sorted(CPUS), default="68000")
    ap.add_argument("--system6", action="store_true",
                    help="no _FlushCodeCache, as on System 6 on a 68000")
    ap.add_argument("--os", action="store_true",
                    help="fake the Memory Manager, the File Manager, "
                    "_PMgrOp and the Sound Driver")
    ap.add_argument("--files", action="store_true",
                    help="with --os, print the files left at the end")
    ap.add_argument("--input", metavar="TEXT",
                    help="with --os, the file \"App Input\" holds TEXT, "
                    "with \\n as the Mac's \\r (the C library's stdin)")
    ap.add_argument("--keys", metavar="TEXT", default="",
                    help="what GetNextEvent types (\\n is Return), then "
                    "Command-period")
    ap.add_argument("--exit-anywhere", action="store_true",
                    help="do not check the stack at ExitToShell (exit() "
                         "calls it from inside the program)")
    ap.add_argument("--mem", type=int, default=4, metavar="MB",
                    help="the Mac's memory (default 4 MB); the heap is "
                    "what the code and the stack leave")
    ap.add_argument("--steps", type=int, default=2_000_000,
                    help="the most instructions to run (default 2000000)")
    a = ap.parse_args()

    code = resources(read_macbinary(a.app), b"CODE")
    code0, code1 = code[0], code[1]
    # A larger Mac: the stack and the return addresses move to the top,
    # and the heap starts after the code.
    if a.mem != 4:
        MEM_SIZE = a.mem << 20
        STACK_TOP = MEM_SIZE - 0x100
        IRQ_RETURN = MEM_SIZE - 0x10
        THREAD_END = MEM_SIZE - 0x20
        HEAP_AT = max(0x200000, (CODE_AT + len(code1) + 0xFFFF) & ~0xFFFF)
        HEAP_END = MEM_SIZE - 0x20000
    above, below, jtsize, jtoff = struct.unpack_from(">IIII", code0, 0)
    entry = struct.unpack_from(">H", code0, 16)[0]

    uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
    uc.ctl_set_cpu_model(CPUS[a.cpu])
    uc.mem_map(MEM_BASE, MEM_SIZE)
    uc.mem_write(CODE_AT, code1)
    seg_start = CODE_AT + 4
    uc.reg_write(UC_M68K_REG_A5, A5_AT)
    uc.reg_write(UC_M68K_REG_A7, STACK_TOP)
    state = {"clicks": a.clicks, "done": False, "log": [], "port": None,
             "keys": key_events(a.keys)}
    fake = FakeOS(uc) if a.os else None
    if fake:
        fake.map_temp(MEM_BASE + MEM_SIZE)
    screen = FakeScreen(fake) if fake else None
    net = FakeNet(fake) if fake else None
    if fake and a.cpu != "68000":
        GESTALT[b"qd  "] = 0x0230           # 32-bit Color QuickDraw 1.3
    # CurApName (0x910): the application's name, from the MacBinary header.
    mb = open(a.app, "rb").read(128)
    uc.mem_write(0x910, mb[1:2 + min(mb[1], 31)])
    if fake and a.input is not None:
        app = mb[2:2 + min(mb[1], 31)].decode("mac_roman")
        fake.files[app[:25] + " Input"] = bytearray(
            a.input.replace("\\n", "\r").encode("mac_roman"))
    # Time (0x20C): the clock, in local seconds since 1904.
    uc.mem_write(0x20C, struct.pack(">I", int(time.time()) + 2082844800 +
                                    (GMT_DELTA if a.os else 0)))

    def rd(addr, n):
        return bytes(uc.mem_read(addr, n))

    def pstring(addr):
        n = rd(addr, 1)[0]
        return rd(addr + 1, n).decode("mac_roman")

    saved = []                  # registers while a completion routine runs
    state["ticks"] = 1000
    state["us"] = 5_000_000_000

    def save():
        return [uc.reg_read(r) for r in REGS]

    def load(regs):
        for r, v in zip(REGS, regs):
            uc.reg_write(r, v)

    threads = FakeThreads(save, load) if fake else None

    def thread_call(pc):
        """_ThreadDispatch: the Pascal arguments are on the stack, the
        selector in D0 gives their size in words (high byte) and the
        call (low byte). The OSErr result is above the arguments."""
        d0 = uc.reg_read(UC_M68K_REG_D0)
        call, size = d0 & 0xFF, 2 * ((d0 >> 8) & 0xFF)
        sp = uc.reg_read(UC_M68K_REG_A7)
        args = rd(sp, size)
        err, switch, end = 0, None, False
        if call == 3:                           # NewThread
            made, _, _, stack, param, entry, _ = struct.unpack(">7I", args)
            stack = stack or THREAD_STACK
            base = fake.alloc(stack)
            if not base:
                err = -108
            else:
                top = base + stack - 12
                uc.mem_write(top, struct.pack(">III", THREAD_END, param, 0))
                regs = save()
                regs[REGS.index(UC_M68K_REG_PC)] = entry
                regs[REGS.index(UC_M68K_REG_A7)] = top
                if made:
                    uc.mem_write(made, struct.pack(">I", threads.new(regs)))
        elif call == 4:                         # DisposeThread
            tid = struct.unpack(">I", args[:4])[0]
            if tid == threads.cur:
                end = True
            elif not threads.dispose(tid):
                err = -618                      # threadNotFoundErr
        elif call == 5:                         # YieldToThread
            switch = struct.unpack(">I", args)[0]
        elif call == 6:                         # GetCurrentThread
            uc.mem_write(struct.unpack(">I", args)[0],
                         struct.pack(">I", threads.cur))
        elif call not in (0x0B, 0x0C):          # Begin/EndCritical
            print(f"Thread Manager call {call:#x} is not faked")
            uc.emu_stop()
            return
        sp += size
        uc.mem_write(sp, struct.pack(">h", err))
        uc.reg_write(UC_M68K_REG_A7, sp)
        uc.reg_write(UC_M68K_REG_PC, pc + 2)
        if end and not threads.end():
            print("sim68k: the last thread ended")
            uc.emu_stop()
        elif switch is not None:
            threads.switch(switch)

    def on_code(uc, pc, size, _):
        if pc == THREAD_END and threads:
            # A thread's entry function returned: the Thread Manager
            # disposes of the thread.
            if not threads.end():
                print("sim68k: the last thread ended")
                uc.emu_stop()
            return
        if pc == IRQ_RETURN and saved:
            # The completion routine returned: go back to the trap it
            # interrupted, with every register as it was.
            for r, v in zip(REGS, saved.pop()):
                uc.reg_write(r, v)
            return
        # Toolbox traps are A-line words: catch them before the CPU runs
        # them, fake them, and continue after the word.
        word = struct.unpack(">H", rd(pc, 2))[0]
        if word & 0xF000 != 0xA000:
            return
        if fake and fake.sound and not saved and not (
                word & 0xF9F8 == 0xA000 and
                struct.unpack(">h", rd(uc.reg_read(UC_M68K_REG_A0) + 24,
                                       2))[0] == -4):
            # A write ends before this trap, like an interrupt: the
            # completion routine gets the block in A0 and the result in D0.
            pb, done = fake.sound_done()
            if done:
                saved.append([uc.reg_read(r) for r in REGS])
                sp = uc.reg_read(UC_M68K_REG_A7) - 4
                uc.mem_write(sp, struct.pack(">I", IRQ_RETURN))
                uc.reg_write(UC_M68K_REG_A7, sp)
                uc.reg_write(UC_M68K_REG_A0, pb)
                uc.reg_write(UC_M68K_REG_D0, 0)
                uc.reg_write(UC_M68K_REG_PC, done)
                return
        if net and word & 0x0800 == 0:
            net.pump()
            pb = uc.reg_read(UC_M68K_REG_A0)
            res = None
            if word & 0xF9FF == 0xA000 and fake.name(fake.r32(pb + 18)) == ".IPP":
                fake.w16(pb + 24, FakeNet.IPP)  # OpenDriver(".IPP")
                fake.w16(pb + 16, 0)
                res = 0
            elif word & 0xF9FF == 0xA004 and fake.r16(pb + 24) == FakeNet.IPP:
                res = net.control(pb, word & 0x0400)
            elif word & 0xFBFF == 0xA0DD:       # _PPC
                res = net.ppc(uc.reg_read(UC_M68K_REG_D0) & 0xFF, pb)
            if res is not None:
                uc.reg_write(UC_M68K_REG_D0, res & 0xFFFFFFFF)
                uc.reg_write(UC_M68K_REG_PC, pc + 2)
                return
        if fake and word & 0x0800 == 0 and (fake.memory(word) or
                                            fake.device(word) or
                                            fake.hfs(word) or
                                            fake.file(word)):
            uc.reg_write(UC_M68K_REG_PC, pc + 2)
            return
        if word == 0xABF2 and threads:
            thread_call(pc)
            return
        if word == 0xA1AD and fake:
            # Gestalt: the selector in D0; the answer in A0, the result in D0.
            sel = struct.pack(">I", uc.reg_read(UC_M68K_REG_D0) & 0xFFFFFFFF)
            uc.reg_write(UC_M68K_REG_A0, GESTALT.get(sel, 0))
            uc.reg_write(UC_M68K_REG_D0, 0 if sel in GESTALT else -5551)
            uc.reg_write(UC_M68K_REG_PC, pc + 2)
            return
        if word == 0xA88F and fake and fake.r16(
                uc.reg_read(UC_M68K_REG_A7)) == 0x1D:
            # TempNewHandle(size, &resultCode): the selector, then the
            # arguments, popped; the Handle result left on the stack.
            sp = uc.reg_read(UC_M68K_REG_A7) & 0xFFFFFFFF
            result_ptr, size = fake.r32(sp + 2), fake.r32(sp + 6)
            h, err = fake.temp_new_handle(size)
            if result_ptr:
                fake.w16(result_ptr, err)
            fake.w32(sp + 10, h)
            uc.reg_write(UC_M68K_REG_A7, sp + 10)
            uc.reg_write(UC_M68K_REG_PC, pc + 2)
            return
        if word == 0xA193 and not a.system6:
            # Microseconds: the high half in A0, the low half in D0.
            state["us"] += 50
            uc.reg_write(UC_M68K_REG_A0, state["us"] >> 32)
            uc.reg_write(UC_M68K_REG_D0, state["us"] & 0xFFFFFFFF)
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
                (name == "GetOSTrapAddress" and num in (0xBD, 0x93) and
                 a.system6)
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
        elif name == "DrawText":
            count, first, buf = struct.unpack(">hhI", args)
            desc = repr(rd(buf + first, count).decode("mac_roman"))
        elif name == "SetRect":
            b, r, t, l, rect = struct.unpack(">hhhhI", args)
            uc.mem_write(rect, struct.pack(">hhhh", t, l, b, r))
            desc = f"{l},{t},{r},{b}"
        elif name == "GetFontInfo":                 # Monaco 9
            uc.mem_write(struct.unpack(">I", args)[0],
                         struct.pack(">hhhh", 9, 2, 6, 0))
        state["log"].append((name, desc))
        if name not in QUIET:
            print(f"{name}({desc})")
        if nargs:
            sp += nargs
        def addr(off):
            return struct.unpack_from(">I", args, off)[0]

        if (name == "NewWindow" or name == "NewCWindow") and screen:
            w = screen.new_window(addr(22), addr(18),
                                  struct.unpack_from(">i", args, 0)[0])
            uc.mem_write(sp, struct.pack(">I", w))
        elif name == "NewWindow" or name == "NewCWindow":
            struct.pack_into(">I", buf := bytearray(4), 0, 0x30000)
            uc.mem_write(sp, bytes(buf))
        elif name == "DisposeWindow" and screen:
            screen.dispose(addr(0))
        elif name == "GetWRefCon" and screen:
            uc.mem_write(sp, struct.pack(">i", screen.refcon(addr(0))))
        elif name == "SetPort" and screen:
            screen.port = addr(0)
        elif name == "GetPort" and screen:
            uc.mem_write(addr(0), struct.pack(">I", screen.port))
        elif name == "GetMouse":
            uc.mem_write(addr(0), bytes(4))
        elif name == "WaitNextEvent" and screen:
            state["ticks"] += 1
            uc.mem_write(sp, bytes([screen.wait_event(addr(8),
                                                      state["ticks"]), 0]))
        elif name == "EventAvail":
            uc.mem_write(addr(0), bytes(16))
            uc.mem_write(sp, bytes(2))
        elif name == "MenuSelect":
            uc.mem_write(sp, bytes(4))
        elif name == "CopyBits" and screen:
            screen.copy_bits(addr(18), addr(14), addr(10), addr(6))
        elif name == "GetNextEvent":
            ch, mods = next(state["keys"])
            ev = struct.pack(">HIIIH", 3, ch, 0, 0, mods)   # keyDown
            uc.mem_write(struct.unpack(">I", args[:4])[0], ev)
            uc.mem_write(sp, b"\x01\x00")
        elif name == "NewRgn":
            uc.mem_write(sp, struct.pack(">I", 0x40000))
        elif name == "NewMenu":
            uc.mem_write(sp, struct.pack(">I", 0x50000))
        elif name == "GetIndResource":          # finds no resource
            uc.mem_write(sp, b"\0\0\0\0")
        elif name == "FindFolder":              # finds no folder
            uc.mem_write(sp, struct.pack(">h", -43))
        elif name == "TickCount":               # one tick per call
            state["ticks"] += 1
            uc.mem_write(sp, struct.pack(">I", state["ticks"]))
        elif name in ("FindWindow", "TrackGoAway"):
            uc.mem_write(sp, b"\x00\x00")
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
        uc.emu_start(seg_start + entry, MEM_SIZE, count=a.steps)
    except UcError as e:
        pc = uc.reg_read(UC_M68K_REG_PC)
        sys.exit(f"sim68k: {e} at {pc:#x} (segment offset "
                 f"{pc - seg_start:#x})")
    # The files, also when the program did not finish: they tell how
    # far it went.
    if fake and a.files:
        for n, data in sorted(fake.files.items()):
            text = bytes(data).replace(b"\r", b"\n").decode("mac_roman")
            print(f"file {n!r}:\n{text}", end="" if text.endswith("\n") else "\n")
    if not state["done"]:
        if screen:
            screen.summary()
        sys.exit("sim68k: the program did not reach ExitToShell")
    if screen and not screen.summary():
        sys.exit(1)
    balance = state["sp"] - STACK_TOP
    print(f"sim68k: reached ExitToShell; stack {'balanced' if balance == 0 else f'off by {balance}'}")
    sys.exit(0 if balance == 0 or a.exit_anywhere else 1)


if __name__ == "__main__":
    main()
