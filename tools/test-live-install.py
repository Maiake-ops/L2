#!/usr/bin/env python3
"""Regression test: `l2install disk` from INSIDE the booted L2 live system.

Boots build/out/l2.iso with an empty virtio target disk, runs the installer
from the L2# shell (tools come from the l2/install pack on the medium), and
checks it completes. Then boots the installed disk to confirm it reaches L2#.

Run from anywhere:  python3 tools/test-live-install.py
Needs: build/out/l2.iso (l2 build iso), qemu-system-x86_64, KVM (or TCG).
"""
import os, re, subprocess, select, sys, time, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "build/out/l2.iso")


def accel():
    return ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else ["-accel", "tcg"]


class Guest:
    def __init__(self, cmd):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.buf = b""

    def read_until(self, tok, sec):
        end = time.monotonic() + sec
        while time.monotonic() < end:
            r, _, _ = select.select([self.p.stdout], [], [], 0.3)
            if r:
                c = os.read(self.p.stdout.fileno(), 65536)
                if not c:
                    break
                self.buf += c
                if tok in self.buf:
                    return True
            if self.p.poll() is not None:
                break
        return tok in self.buf

    def send(self, line):
        time.sleep(0.5)
        self.p.stdin.write(line.encode() + b"\r\n")
        self.p.stdin.flush()

    def close(self):
        try:
            if self.p.poll() is None:
                self.p.kill()
        except Exception:
            pass
        try:
            self.p.wait(timeout=10)
        except Exception:
            pass
        for fh in (self.p.stdin, self.p.stdout):
            if fh is not None:
                try:
                    fh.close()
                except OSError:
                    pass


def main():
    if not os.path.exists(ISO):
        sys.exit("missing build/out/l2.iso - run: l2 build iso")
    tmp = tempfile.mkdtemp(prefix="l2-live-install-")
    target = os.path.join(tmp, "target.img")
    subprocess.run(["truncate", "-s", "256M", target], check=True)
    base = ["qemu-system-x86_64", "-m", "256", "-smp", "2", "-display", "none",
            "-serial", "stdio", "-monitor", "none", "-no-reboot"] + accel()
    try:
        # 1. install from the live system
        g = Guest(base + ["-cdrom", ISO, "-boot", "d",
                          "-drive", f"file={target},format=raw,if=virtio"])
        try:
            if not g.read_until(b"L2#", 120):
                print("FAIL: live system did not reach L2#")
                return 1
            time.sleep(8)
            g.buf = b""
            # split marker so the literal only appears in the output, not the echo
            g.send("l2install -y --no-wifi disk /dev/vda; echo INSTALL_''RC=$?")
            if not g.read_until(b"INSTALL_RC=", 240):
                print("FAIL: installer did not finish")
                print(g.buf[-2000:].decode(errors="replace"))
                return 1
            time.sleep(1)
            end = time.monotonic() + 2
            while time.monotonic() < end:
                r, _, _ = select.select([g.p.stdout], [], [], 0.3)
                if r:
                    c = os.read(g.p.stdout.fileno(), 65536)
                    if not c:
                        break
                    g.buf += c
            out = g.buf.decode(errors="replace")
        finally:
            g.close()
        if "INSTALL_RC=0" not in out:
            print("FAIL: installer returned non-zero")
            print(out[-2000:])
            return 1
        print("PASS: l2install disk completed from the live system (RC=0)")

        # 2. boot the installed disk and verify ext4 root + clean poweroff
        g = Guest(base + ["-drive", f"file={target},format=raw,if=virtio"])
        try:
            if not g.read_until(b"L2#", 120):
                print("FAIL: installed disk did not boot to L2#")
                return 1
            g.buf = b""
            g.send("test -f /etc/l2/installed && grep '^/dev/[^ ]* / ext4 ' /proc/mounts; echo ROOT_''DONE")
            if not g.read_until(b"ROOT_DONE", 20):
                print("FAIL: installed disk shell probe timed out")
                return 1
            root_out = g.buf.decode(errors="replace")
            if not re.search(r"^/dev/\S+ / ext4 ", root_out, re.MULTILINE):
                print("FAIL: installed ext4 root not mounted")
                return 1
            g.send("poweroff")
            try:
                status = g.p.wait(timeout=60)
            except subprocess.TimeoutExpired:
                print("FAIL: installed disk did not power off cleanly")
                return 1
            if status != 0:
                print(f"FAIL: installed disk exited with status {status}")
                return 1
        finally:
            g.close()
        print("PASS: installed disk boots ext4 root to L2# and powers off cleanly")
        return 0
    finally:
        subprocess.run(["rm", "-rf", tmp], check=False)


if __name__ == "__main__":
    sys.exit(main())
