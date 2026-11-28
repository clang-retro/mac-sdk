#!/usr/bin/env python3
"""Run a network test program on Mac OS in QEMU, and collect its report.

The script puts the program in the Startup Items folder of a copy of a
Mac OS disk or CD, so that Mac OS starts it after it boots. It starts two
servers on the host, which the guest finds at 10.0.2.2 (QEMU's user
network):

  port 7009  a report server: it keeps what the program sends
  port 8080  a web server: it gives a short page to each GET

Then it boots QEMU and waits for the report to end (the program closes
the connection), or for a request with --until-request.

    # PowerPC: Mac OS 9.2.1 from its install CD
    python3 tools/qemu/nettest.py --machine mac99 --image "Mac OS 9.2.1.iso" ot-ppc.bin

    # 68k: Mac OS 8.1 on a Quadra 800 disk (needs the Quadra 800 ROM)
    python3 tools/qemu/nettest.py --machine q800 --rom q800.rom \\
        --image hd81.qcow2 ot-68k.bin

    # A program that reports nothing: wait for its web requests
    python3 tools/qemu/nettest.py --machine mac99 --image os921.iso \\
        --until-request "BSD sockets" --shot fetch.png Fetch.bin

The exit status is 0 when the report has "all ok" (or the request came),
1 when it has "FAILED" or the time ends. tests/ot.c and examples/net/
fetch.c know these servers. The guest needs TCP/IP set to DHCP (the
default of the install CDs).

Needs qemu-system-ppc or qemu-system-m68k, qemu-img and hfsutils.
"""
import argparse
import http.server
import os
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))


class Report:
    """Keeps the bytes that the guest sends to the report server."""

    def __init__(self):
        """Makes an empty report."""
        self.text = ""
        self.ended = False
        self.requests = []
        self.lock = threading.Lock()

    def add(self, data):
        """Adds bytes from the guest to the report."""
        with self.lock:
            self.text += data.decode("latin-1")

    def end(self):
        """Records that the guest closed the report connection."""
        with self.lock:
            self.ended = True


def serve_reports(report, port):
    """Starts the report server on a thread. Gives the server."""
    class Handler(socketserver.BaseRequestHandler):
        """Reads one report connection until the guest closes it."""

        def handle(self):
            """Adds each block that comes to the report."""
            while True:
                data = self.request.recv(4096)
                if not data:
                    break
                report.add(data)
            report.end()

    socketserver.TCPServer.allow_reuse_address = True
    server = socketserver.ThreadingTCPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def serve_web(report, port):
    """Starts the web server on a thread. Gives the server."""
    class Handler(http.server.BaseHTTPRequestHandler):
        """Gives a short page, and records the User-Agent of each GET."""

        def do_GET(self):
            """Sends the page."""
            body = b"<html><body>Hello from the QEMU host.</body></html>\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            with report.lock:
                report.requests.append(self.headers.get("User-Agent", ""))

        def log_message(self, *args):
            """Writes no log."""

    http.server.HTTPServer.allow_reuse_address = True
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def run(cmd, env=None):
    """Runs a command, and stops the script when it fails."""
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, env=env)


def install(image, app, name, work):
    """Copies a MacBinary program into the Startup Items folder of the
    first HFS volume of a raw disk image. hfsutils keeps its current
    volume in $HOME/.hcwd, so each run gets its own HOME."""
    env = dict(os.environ, HOME=work)
    run(["hmount", image], env)
    try:
        # hmkdir fails when the folder is there: that is correct. (hls
        # gives status 0 also for a missing folder, so it cannot tell.)
        subprocess.run(["hmkdir", ":System Folder:Startup Items"], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        run(["hcopy", "-m", app, ":System Folder:Startup Items:" + name], env)
    finally:
        subprocess.run(["humount"], stdout=subprocess.DEVNULL, env=env)


def qemu_command(a, image, work):
    """Gives the QEMU command for the machine."""
    mon = ["-monitor", f"unix:{work}/mon.sock,server,nowait",
           "-display", "none", "-vnc", a.vnc]
    if a.machine == "mac99":
        cd = a.image.lower().endswith((".iso", ".toast", ".cdr"))
        drive = (["-drive", f"file={image},format=raw,media=cdrom", "-boot", "d"] if cd else
                 ["-drive", f"file={image},format=raw,media=disk", "-boot", "c"])
        return (["qemu-system-ppc", "-M", "mac99,via=pmu", "-m", "512"] + drive +
                ["-netdev", "user,id=n0", "-device", "sungem,netdev=n0",
                 "-prom-env", "auto-boot?=true"] + mon)
    if not a.rom:
        sys.exit("nettest: --machine q800 needs --rom (a Quadra 800 ROM image)")
    return (["qemu-system-m68k", "-M", "q800", "-m", "128", "-bios", a.rom,
             "-g", "800x600x8",
             "-device", "scsi-hd,scsi-id=0,drive=hd0",
             "-drive", f"file={image},format=raw,if=none,id=hd0",
             "-nic", "user,model=dp83932",
             "-audiodev", "none,id=snd", "-machine", "audiodev=snd"] + mon)


def screenshot(work, path):
    """Saves the guest's screen as a PNG file (needs Pillow)."""
    s = socket.socket(socket.AF_UNIX)
    s.connect(os.path.join(work, "mon.sock"))
    ppm = os.path.join(work, "screen.ppm")
    s.sendall(f"screendump {ppm}\n".encode())
    time.sleep(1.5)
    s.close()
    from PIL import Image
    Image.open(ppm).save(path)


def main():
    """Prepares the disk, starts the servers and QEMU, and waits."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("app", help="the program, a MacBinary file")
    ap.add_argument("--machine", choices=["mac99", "q800"], required=True)
    ap.add_argument("--image", required=True,
                    help="a Mac OS install CD or disk (raw, ISO or qcow2); "
                         "the script works on a copy")
    ap.add_argument("--rom", help="the Quadra 800 ROM (q800 only)")
    ap.add_argument("--timeout", type=int, default=600, help="seconds")
    ap.add_argument("--until-request", metavar="TEXT",
                    help="end at a web request whose User-Agent has TEXT")
    ap.add_argument("--shot", metavar="PNG", help="save the screen at the end")
    ap.add_argument("--vnc", default=":3", help="the VNC display (default :3)")
    ap.add_argument("--report-port", type=int, default=7009)
    ap.add_argument("--web-port", type=int, default=8080)
    a = ap.parse_args()

    work = tempfile.mkdtemp(prefix="nettest.")
    proc = None
    try:
        image = os.path.join(work, "disk.img")
        run(["qemu-img", "convert", "-O", "raw", a.image, image])
        install(image, a.app, "Net Test", work)
        report = Report()
        servers = [serve_reports(report, a.report_port), serve_web(report, a.web_port)]
        proc = subprocess.Popen(qemu_command(a, image, work),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        end = time.time() + a.timeout
        while time.time() < end and proc.poll() is None:
            with report.lock:
                if a.until_request:
                    if any(a.until_request in r for r in report.requests):
                        break
                elif report.ended or "all ok" in report.text:
                    break
            time.sleep(2)
        if a.shot and proc.poll() is None:
            time.sleep(5)
            screenshot(work, a.shot)
        for s in servers:
            s.shutdown()
        sys.stdout.write(report.text)
        for r in report.requests:
            print(f"GET from {r}")
        if a.until_request:
            ok = any(a.until_request in r for r in report.requests)
        else:
            ok = "all ok" in report.text and "FAILED" not in report.text
        if proc.poll() is not None:
            print(f"nettest: QEMU stopped: {proc.stderr.read().decode(errors='replace')}")
        elif not ok:
            print("nettest: the time ended" if time.time() >= end else "nettest: failed")
        return 0 if ok else 1
    finally:
        if proc and proc.poll() is None:
            proc.kill()
            proc.wait()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
