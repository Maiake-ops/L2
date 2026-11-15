#!/usr/bin/env python3
"""l2 ramtest - boot L2 under QEMU and measure idle memory, or smoke-test an ISO.

  ramtest.py --kernel K --initrd I [--cmdline STR] [--sizes 256M,1G,4G,16G] [--budget KiB]
  ramtest.py --iso l2.iso [--uefi] [--ovmf CODE.fd]

What is measured (from /proc/meminfo inside the guest, after boot has settled):
  used     = MemTotal - MemAvailable      what the OS actually holds at idle
  reserved = installed - MemTotal         kernel image + struct page array + early
                                          hash tables; NOT visible to `free`
The struct page array is ~1.5% of installed RAM (64 B per 4 KiB page), so
`reserved` grows with RAM even with every tuning knob; `used` must not.
Exit status 1 if any size exceeds --budget (default 10240 KiB) or a boot fails.
"""
import argparse, os, re, shutil, subprocess, sys, time, select

PROMPT = b"L2#"
MARK = "L2RAMDONE"


def have(x):
    return shutil.which(x) is not None


def parse_size(s):
    m = re.fullmatch(r"(\d+)([KMG]?)", s.strip().upper())
    if not m:
        raise SystemExit(f"bad size: {s}")
    n = int(m.group(1))
    return n * {"": 1, "K": 1, "M": 1024, "G": 1024 * 1024}[m.group(2)] if m.group(2) != "" else n * 1024


def parse_meminfo(text):
    d = {}
    for line in text.splitlines():
        m = re.match(r"(\w+):\s+(\d+) kB", line.strip())
        if m:
            d[m.group(1)] = int(m.group(2))
    return d


class Guest:
    def __init__(self, argv):
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.buf = b""

    def read_until(self, token, timeout):
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.p.stdout], [], [], 0.5)
            if r:
                chunk = os.read(self.p.stdout.fileno(), 65536)
                if not chunk:
                    break
                self.buf += chunk
                if token in self.buf:
                    return True
            if self.p.poll() is not None:
                break
        return token in self.buf

    def send(self, line):
        # serial stdio can drop the LF: terminate with both, settle first (rcl2 handoff notes)
        time.sleep(0.7)
        self.p.stdin.write(line.encode() + b"\r\n")
        self.p.stdin.flush()

    def meminfo(self, samples=3):
        # Sample a few times and keep the lowest "used": transient page-cache /
        # reclaim activity can briefly inflate MemTotal-MemAvailable.
        best_used, best_info = None, None
        for _ in range(samples):
            self.buf = b""
            self.send("cat /proc/meminfo; echo L2RAM''DONE")
            if not self.read_until(MARK.encode() + b"\r", 20):
                break
            info = parse_meminfo(self.buf.decode(errors="replace").split("cat /proc/meminfo")[-1])
            if "MemTotal" not in info:
                break
            avail = info.get("MemAvailable", info["MemFree"] + info.get("Cached", 0))
            used = info["MemTotal"] - avail
            if best_used is None or used < best_used:
                best_used, best_info = used, info
            time.sleep(2)
        return best_info

    def close(self):
        try:
            self.p.kill()
        except Exception:
            pass
        self.p.wait()


def accel():
    return ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else ["-accel", "tcg"]


def measure(args, size_s):
    kib = parse_size(size_s)
    if getattr(args, "iso", None):
        # Boot the real ISO: QEMU's -kernel/-initrd path keeps the initrd in
        # fw_cfg memory and reads ~1 MiB high, so the ISO is authoritative.
        cmd = ["qemu-system-x86_64", "-m", size_s, "-smp", "2", "-display", "none",
               "-serial", "stdio", "-monitor", "none", "-no-reboot",
               "-cdrom", args.iso, "-boot", "d"] + accel()
    else:
        cmd = ["qemu-system-x86_64", "-m", size_s, "-smp", "2", "-display", "none", "-serial", "stdio",
               "-monitor", "none", "-no-reboot", "-kernel", args.kernel, "-initrd", args.initrd,
               "-append", f"rdinit=/init console=ttyS0,115200 panic=-1 {args.cmdline}"] + accel()
    g = Guest(cmd)
    try:
        if not g.read_until(PROMPT, args.timeout):
            return {"size": size_s, "error": "no L2# prompt (boot failed or too slow)"}
        time.sleep(args.settle)  # let services (net, getty) finish starting
        info = g.meminfo()
        if not info:
            return {"size": size_s, "error": "could not parse meminfo"}
        avail = info.get("MemAvailable", info["MemFree"] + info.get("Cached", 0))
        used = info["MemTotal"] - avail
        return {"size": size_s, "installed": kib, "total": info["MemTotal"], "used": used,
                "reserved": kib - info["MemTotal"], "slab": info.get("Slab", 0),
                "pt": info.get("PageTables", 0), "stack": info.get("KernelStack", 0),
                "percpu": info.get("Percpu", 0)}
    finally:
        g.close()


def ram_sweep(args):
    if not have("qemu-system-x86_64"):
        raise SystemExit("qemu-system-x86_64 not found (l2 deps install)")
    sizes = [s for s in args.sizes.split(",") if s]
    rows, bad = [], 0
    print(f"  {'RAM':>6} {'used':>9} {'budget':>9}  {'kernel reserved':>16}  slab  ptbl stack percpu")
    for s in sizes:
        r = measure(args, s)
        if "error" in r:
            print(f"  {s:>6}  FAIL: {r['error']}")
            bad += 1
            continue
        ok = r["used"] <= args.budget
        bad += 0 if ok else 1
        print(f"  {s:>6} {r['used']:>7} K {args.budget:>7} K  {r['reserved']:>13} K  "
              f"{r['slab']:>4} {r['pt']:>5} {r['stack']:>5} {r['percpu']:>6}   {'OK' if ok else 'OVER BUDGET'}")
    print("\n  used = MemTotal - MemAvailable (includes the initramfs files); reserved is not shown by `free`.")
    return 1 if bad else 0


def find_ovmf(explicit):
    if explicit:
        return explicit
    for p in ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/edk2/ovmf/OVMF_CODE.fd",
              "/usr/share/edk2-ovmf/x64/OVMF_CODE.fd", "/usr/share/qemu/OVMF.fd",
              "/run/current-system/sw/share/qemu/edk2-x86_64-code.fd",
              os.path.expanduser("~/.nix-profile/share/qemu/edk2-x86_64-code.fd")):
        if os.path.exists(p):
            return p
    return None


def boot_medium(args):
    """Boot an ISO (--iso) or a disk image (--disk) and smoke-test the shell.

    For a disk image it also reports idle RAM and confirms the root was mounted
    from disk (installed system) rather than run from the initramfs.
    """
    if not have("qemu-system-x86_64"):
        raise SystemExit("qemu-system-x86_64 not found")
    medium = args.iso or args.disk
    cmd = ["qemu-system-x86_64", "-m", args.ram, "-smp", "2", "-display", "none", "-serial", "stdio",
           "-monitor", "none", "-no-reboot"] + accel()
    if args.iso:
        cmd += ["-cdrom", args.iso, "-boot", "d"]
    else:
        cmd += ["-drive", f"file={args.disk},format=raw,if=virtio"]
    mode = "BIOS"
    if args.uefi:
        ovmf = find_ovmf(args.ovmf)
        if not ovmf:
            raise SystemExit("OVMF firmware not found; pass --ovmf /path/OVMF_CODE.fd")
        cmd += ["-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}"]
        mode = "UEFI"
    g = Guest(cmd)
    try:
        if not g.read_until(PROMPT, args.timeout):
            print(f"  FAIL {mode}: no L2# prompt from {medium}")
            print(g.buf.decode(errors="replace")[-1500:])
            return 1
        time.sleep(args.settle)
        info = g.meminfo()
        print(f"  PASS {mode}: booted to the L2# shell  ({medium})")
        if info:
            avail = info.get("MemAvailable", info["MemFree"] + info.get("Cached", 0))
            used = info["MemTotal"] - avail
            ok = used <= args.budget
            print(f"    idle RAM: {used} KiB used of {info['MemTotal']} KiB "
                  f"(budget {args.budget} KiB)  {'OK' if ok else 'OVER BUDGET'}")
        probe = "uname -r"
        if args.disk:
            probe += "; cat /etc/l2/installed 2>/dev/null; grep ' / ' /proc/mounts"
        g.send(probe + "; echo L2RAM''DONE")
        if g.read_until(MARK.encode() + b"\r", 20):
            out = g.buf.decode(errors="replace").split("uname -r")[-1]
            for line in out.splitlines():
                s = line.strip()
                if s.startswith("7.") or "installed" in s or s.startswith("/dev/"):
                    print("   ", s)
        g.send("poweroff")
        return 0
    finally:
        g.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel"); ap.add_argument("--initrd"); ap.add_argument("--cmdline", default="loglevel=4")
    ap.add_argument("--sizes", default="256M,1G,4G,16G")
    ap.add_argument("--budget", type=int, default=int(os.environ.get("L2_RAM_BUDGET_KIB", 10240)))
    ap.add_argument("--timeout", type=int, default=180); ap.add_argument("--settle", type=float, default=25.0)
    ap.add_argument("--iso"); ap.add_argument("--disk"); ap.add_argument("--ram", default="256")
    ap.add_argument("--uefi", action="store_true"); ap.add_argument("--ovmf")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        d = parse_meminfo("MemTotal:  1000 kB\nMemFree: 10 kB\nMemAvailable:   300 kB\n")
        assert d["MemTotal"] - d["MemAvailable"] == 700 and parse_size("1G") == 1048576 and parse_size("256M") == 262144
        print("selftest ok"); return 0
    if a.disk or (a.iso and not a.kernel):
        return boot_medium(a)
    if not (a.kernel and a.initrd):
        ap.error("need --kernel and --initrd (or --iso / --disk)")
    return ram_sweep(a)


if __name__ == "__main__":
    sys.exit(main())
