#!/usr/bin/env python3
"""Regression test: `l2install disk` from INSIDE the booted L2 live system.

Boots build/out/l2.iso with an empty virtio target disk, runs the installer
from the L2# shell (tools come from the l2/install pack on the medium), and
checks it completes. Then boots the installed disk to confirm it reaches L2#.

Run from anywhere:  python3 tools/test-live-install.py
Needs: build/out/l2.iso (l2 build iso), qemu-system-x86_64, KVM (or TCG).
"""
import os, subprocess, select, sys, time, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build/out/l2.iso")
if not os.path.exists(ISO):
    sys.exit("missing build/out/l2.iso - run: l2 build iso")

def accel():
    return ["-accel", "kvm"] if os.path.exists("/dev/kvm") else ["-accel", "tcg"]

class Guest:
    def __init__(self, cmd):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.buf = b""
    def read_until(self, tok, sec):
        end = time.time() + sec
        while time.time() < end:
            r, _, _ = select.select([self.p.stdout], [], [], 0.3)
            if r:
                c = os.read(self.p.stdout.fileno(), 65536)
                if not c: break
                self.buf += c
                if tok in self.buf: return True
            if self.p.poll() is not None: break
        return tok in self.buf
    def send(self, line):
        time.sleep(0.5); self.p.stdin.write(line.encode() + b"\r\n"); self.p.stdin.flush()
    def close(self):
        try: self.p.kill()
        except Exception: pass
        self.p.wait()

def main():
    tmp = tempfile.mkdtemp(prefix="l2-live-install-")
    target = os.path.join(tmp, "target.img")
    subprocess.run(["truncate", "-s", "256M", target], check=True)
    base = ["qemu-system-x86_64", "-m", "256", "-smp", "2", "-display", "none",
            "-serial", "stdio", "-monitor", "none", "-no-reboot"] + accel()
    rc = 1
    try:
        # 1. install from the live system
        g = Guest(base + ["-cdrom", ISO, "-boot", "d",
                          "-drive", f"file={target},format=raw,if=virtio"])
        if not g.read_until(b"L2#", 120):
            print("FAIL: live system did not reach L2#"); return 1
        time.sleep(8); g.buf = b""
        # split marker so the literal only appears in the output, not the echo
        g.send("l2install -y --no-wifi disk /dev/vda; echo INSTALL_''RC=$?")
        if not g.read_until(b"INSTALL_RC=", 240):
            print("FAIL: installer did not finish"); print(g.buf[-2000:].decode(errors="replace")); return 1
        time.sleep(1)
        end = time.time() + 2
        while time.time() < end:
            r, _, _ = select.select([g.p.stdout], [], [], 0.3)
            if r:
                c = os.read(g.p.stdout.fileno(), 65536)
                if not c: break
                g.buf += c
        out = g.buf.decode(errors="replace")
        g.close()
        if "INSTALL_RC=0" not in out:
            print("FAIL: installer returned non-zero"); print(out[-2000:]); return 1
        print("PASS: l2install disk completed from the live system (RC=0)")
        # 2. boot the installed disk
        g = Guest(base + ["-drive", f"file={target},format=raw,if=virtio"])
        ok = g.read_until(b"L2#", 120)
        g.close()
        if not ok:
            print("FAIL: installed disk did not boot to L2#"); return 1
        print("PASS: installed disk boots to L2#")
        rc = 0
        return 0
    finally:
        subprocess.run(["rm", "-rf", tmp], check=False)
        sys.exit(rc) if rc else None

if __name__ == "__main__":
    main()