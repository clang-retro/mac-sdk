#!/usr/bin/env python3
"""Boot each System in MAME with an application in place of its Finder,
take a screenshot, and copy back any "Trap Probe" file the application
wrote (mac-sdk's tools/probe/trapprobe.c).

    matrix.py APP.bin OUTDIR [label ...]

MAC_SYSTEMS is the directory with the System disk images (DiskCopy 4.2,
named as on Apple's Developer CDs), MAME_ROMS the ROM sets (mkroms.py),
MAME the emulator (default: mame on the PATH), JOBS how many at once.
The 400K MFS disks are edited with mfs.py, the others with hfsutils.
"""
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys

import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SYS = os.environ.get("MAC_SYSTEMS", ".") + "/"
ROMS = os.environ.get("MAME_ROMS", os.path.join(HERE, "roms"))
MAME_BIN = os.environ.get("MAME", "mame")
WORK = tempfile.mkdtemp(prefix="matrix.")

# label, disk image, MAME machine, seconds to run
RUNS = [
    ("0.1", "System 0.1", "mac128k", 30),
    ("0.3", "System 0.3", "mac128k", 30),
    ("0.5", "System 0.5", "mac128k", 30),
    ("0.7", "System 0.7", "macplus", 40),
    ("1.0", "System 1.0__1.0 System Tools 512 & 128", "mac512k", 30),
    ("1.0e", "System 1.0__1.0 System Tools 512Ke", "macplus", 40),
    ("1.1", "System 1.1", "macplus", 40),
    ("2.0", "System 2.0__2.0 System Tools", "macplus", 45),
    ("2.0.1", "System 2.0.1__2.0.1 System Tools", "macplus", 45),
    ("5.0", "System 5.0__5.0 System Tools #1", "macplus", 50),
    ("5.1", "System 5.1__5.1 System Tools #1", "macplus", 50),
    ("6.0", "System 6.0__6.0 System Tools", "macplus", 50),
    ("6.0.2", "System 6.0.2__6.0.2 System Tools", "macplus", 50),
    ("6.0.3", "System 6.0.3__6.0.3 System Tools", "macplus", 50),
    ("6.0.4", "System 6.0.4__6.0.4 System Tools", "macplus", 50),
    ("6.0.5", "System 6.0.5__6.0.5 System Tools", "macplus", 50),
    ("6.0.8", "System 6.0.8 Tools.dc42", "macplus", 50),
    ("5.0-II", "System 5.0__5.0 System Tools #1", "macii", 60),
    ("6.0.5-II", "System 6.0.5__6.0.5 System Tools", "macii", 60),
    ("6.0.8-II", "System 6.0.8 Tools.dc42", "macii", 60),
    # System 7.0 and later do not fit on an 800K disk: see harddisk.sh.
]


def is_mfs(raw):
    with open(raw, "rb") as f:
        f.seek(0x400)
        return f.read(2) == b"\xd2\xd7"


def run(job, app, outdir):
    label, img, machine, secs = job
    w = os.path.join(WORK, label)
    shutil.rmtree(w, ignore_errors=True)
    os.makedirs(w)
    env = dict(os.environ, HOME=w)          # hfsutils keeps its state in $HOME
    raw, disk = os.path.join(w, "boot.raw"), os.path.join(w, "boot.dc42")
    subprocess.run(["python3", f"{HERE}/dc42.py", "raw", SYS + img, raw], check=True)
    sh = lambda *a: subprocess.run(list(a), env=env, capture_output=True)
    if is_mfs(raw):
        for f in ["Installer", "Font/DA Mover", "Hard Disk 20", "Scrapbook File",
                  "128K Disk Copy", "Note Pad File", "Imagewriter", "ImageWriter",
                  "Laser Prep", "LaserWriter", "External Drive", "Mac Plus Update",
                  "Finder", "Trap Probe"]:
            sh("python3", f"{HERE}/mfs.py", "rm", raw, f)
        sh("python3", f"{HERE}/mfs.py", "put", raw, "Finder", app)
    else:
        sh("hmount", raw)
        sf = ":System Folder"
        if sh("hls", sf).returncode:
            sf = ""
        for f in ["Installer", "Installer Script", "Read Me", "Apple HD SC Setup",
                  "TeachText", "Disk First Aid", "Font/DA Mover", "HD SC Setup",
                  "Disk Tools Read Me"]:
            sh("hdel", ":" + f)
        for f in ["Finder", "MultiFinder", "Trap Probe"]:
            sh("hdel", f"{sf}:{f}")
        r = sh("hcopy", "-m", app, f"{sf}:Finder")
        if r.returncode:
            return label, "hcopy failed: " + r.stderr.decode()
        sh("humount")
    subprocess.run(["python3", f"{HERE}/dc42.py", "pack", raw, SYS + img, disk], check=True)
    steps = os.path.join(w, "steps.lua")
    open(steps, "w").write(f"return {{ {{{secs}, 'shot'}} }}")
    snap = os.path.join(w, "snap")
    subprocess.run(
        ["timeout", "900", MAME_BIN, machine, "-rompath", ROMS,
         "-flop1", disk, "-video", "none", "-sound", "none", "-nothrottle",
         "-seconds_to_run", str(secs + 1), "-snapshot_directory", snap,
         "-cfg_directory", os.path.join(w, "cfg"), "-nvram_directory", os.path.join(w, "nv"),
         "-autoboot_script", f"{HERE}/drive.lua", "-skip_gameinfo"],
        env=dict(env, MACSTEPS=steps), capture_output=True, cwd=w)
    shots = sorted(os.path.join(dp, f) for dp, _, fs in os.walk(snap) for f in fs)
    if shots:
        shutil.copy(shots[-1], os.path.join(outdir, f"{label}.png"))
    subprocess.run(["python3", f"{HERE}/dc42.py", "raw", disk, raw], check=True)
    out = os.path.join(outdir, f"{label}.trp")
    if is_mfs(raw):
        sh("python3", f"{HERE}/mfs.py", "get", raw, "Trap Probe", out)
    else:
        sh("hmount", raw)
        sh("hcopy", "-r", ":Trap Probe", out)
        sh("hcopy", "-r", ":System Folder:Trap Probe", out)
        sh("humount")
    return label, ("shot" if shots else "no shot") + (", probe" if os.path.exists(out) and os.path.getsize(out) else "")


def main():
    app, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    jobs = [j for j in RUNS if len(sys.argv) < 4 or j[0] in sys.argv[3:]]
    with cf.ThreadPoolExecutor(int(os.environ.get("JOBS", "6"))) as ex:
        for label, res in ex.map(lambda j: run(j, app, outdir), jobs):
            print(label, res, flush=True)


if __name__ == "__main__":
    main()
