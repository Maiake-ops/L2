#!/usr/bin/env python3
"""l2 lowram_matrix - reproducible low-RAM boot matrix with full serial forensics.

Boots L2 at a sweep of guest RAM sizes (default 10..256 MiB) in BIOS/UEFI (ISO,
authoritative) and optionally 'direct' (-kernel/-initrd, labeled comparison only:
fw_cfg keeps the initrd in guest memory and inflates readings ~1 MiB).

Per run: capture the COMPLETE serial log incrementally, wait for the L2# prompt,
settle >= 40 s, sample /proc/meminfo 3x (keep lowest used = MemTotal-MemAvailable),
probe uname/cmdline and fork+exec+shell (/bin/dash spawn), then poweroff.
Failures are classified from the full log (first matching signature wins).

Outputs <outdir>/<size>M-<mode>.log, results.json, summary.md.
Exit 0 only if no FAIL (SKIPPED allowed).  Stdlib only; offline --selftest.

Modelled on tools/ramtest.py (serial framing rules: CRLF sends, 0.7 s settle
before typing, broken echo markers so the echoed command never fakes completion).
"""
import argparse
import json
import os
import re
import select
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PROMPT = b"L2#"
FORENSICS_CMDLINE = "memblock=debug earlyprintk=serial,ttyS0,115200 ignore_loglevel"
DEFAULT_SIZES = "10,12,14,16,20,24,32,64,128,256"
OVMF_PATHS = (
    "/usr/share/OVMF/OVMF_CODE.fd",
    "/usr/share/OVMF/OVMF_CODE_4M.fd",
    "/usr/share/edk2/ovmf/OVMF_CODE.fd",
    "/usr/share/edk2-ovmf/x64/OVMF_CODE.fd",
    "/usr/share/qemu/OVMF.fd",
    "/run/current-system/sw/share/qemu/edk2-x86_64-code.fd",
    "~/.nix-profile/share/qemu/edk2-x86_64-code.fd",
)

# First match wins; searched case-insensitively over the ANSI-stripped log.
# limine-panic is FIRST: its text contains "Out of memory", which would
# otherwise be misclassified as a kernel oom-kill. Limine draws its panic one
# char at a time with cursor-position escapes; after ANSI stripping it reads
# "PANIC: High memory allocator: Out of memory ... End of trace. System halted."
SIGNATURES = (
    ("limine-panic", ("end of trace. system halted", "panic: high memory allocator")),
    ("no-cpio-magic", ("no cpio magic",)),
    ("oom-deadlock", ("system is deadlocked on memory",)),
    ("oom-kill", ("out of memory", "oom-kill")),
    ("decompress-fail", ("decompressor failed",
                         "invalid magic at start of compressed",
                         "failed to execute /init")),
    ("no-init", ("no working init found", "attempted to kill init",
                 "requested init", "failed to execute /init")),
    ("panic-other", ("kernel panic",)),
)

_ANSI = re.compile(r"\x1b\[[0-9;=?]*[A-Za-z]|\x1b\][^\x07]*\x07")


def strip_ansi(text):
    return _ANSI.sub("", text)


# ---------------------------------------------------------------- pure helpers

def parse_sizes(spec):
    """'10,12,16M' -> [10, 12, 16] (positive MiB integers)."""
    out = []
    for tok in spec.split(","):
        tok = tok.strip()
        if not tok:
            continue
        m = re.fullmatch(r"(\d+)[Mm]?", tok)
        if not m or int(m.group(1)) <= 0:
            raise ValueError(f"bad size (want positive MiB integers): {tok!r}")
        out.append(int(m.group(1)))
    if not out:
        raise ValueError("no sizes given")
    return out


def resolve_modes(spec, direct_available):
    modes = list(dict.fromkeys(m.strip().lower() for m in spec.split(",") if m.strip()))
    for m in modes:
        if m not in ("bios", "uefi", "direct"):
            raise ValueError(f"bad mode: {m!r} (want bios,uefi,direct)")
    if not modes:
        raise ValueError("no modes given")
    if direct_available and "direct" not in modes:
        modes.append("direct")
    if "direct" in modes and not direct_available:
        raise ValueError("direct mode needs --kernel and --initrd")
    return modes


def parse_meminfo(text):
    d = {}
    for line in text.splitlines():
        m = re.match(r"(\w+):\s+(\d+) kB", line.strip())
        if m:
            d[m.group(1)] = int(m.group(2))
    return d


def used_kib(info):
    """used = MemTotal - MemAvailable (fallback MemFree+Cached, like ramtest)."""
    if "MemTotal" not in info:
        raise ValueError("meminfo has no MemTotal")
    avail = info.get("MemAvailable", info.get("MemFree", 0) + info.get("Cached", 0))
    return info["MemTotal"] - avail


def classify(log):
    low = strip_ansi(log).lower()
    for name, pats in SIGNATURES:
        if any(p in low for p in pats):
            return name
    if len(log) < 32:
        return "silent"
    return "timeout-no-prompt"


def last_timestamp(log):
    ts = re.findall(r"\[\s*(\d+\.\d+)\]", log)
    return float(ts[-1]) if ts else None


def log_tail(log, n=15):
    return log.splitlines()[-n:]


def split_marker(mark):
    """L2MEMDONE -> L2MEM''DONE: shell concatenates, echoed input does not match."""
    i = len(mark) // 2
    return mark[:i] + "''" + mark[i:]


def probe_line(cmd, mark):
    return f"{cmd}; echo {split_marker(mark)}"


def after_echo(text, cmd):
    """Serial echoes the typed command; keep only what follows its last copy."""
    return text.split(cmd)[-1] if cmd in text else text


def first_output_line(text, mark):
    for line in text.splitlines():
        s = line.strip()
        if s and not s.startswith(";") and "echo" not in s and mark not in s:
            return s
    return None


def find_ovmf(explicit):
    if explicit:
        return explicit
    for p in OVMF_PATHS:
        p = os.path.expanduser(p)
        if os.path.exists(p):
            return p
    return None


def accel():
    return ["-accel", "kvm"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else ["-accel", "tcg"]


def build_cmd(size, mode, smp, iso=None, kernel=None, initrd=None,
              cmdline="", ovmf=None, forensics=False):
    cmd = ["qemu-system-x86_64", "-m", f"{size}M", "-smp", str(smp),
           "-display", "none", "-serial", "stdio", "-monitor", "none",
           "-no-reboot", "-nic", "none"] + accel()
    if mode == "direct":
        append = f"rdinit=/init console=ttyS0,115200 panic=-1 {cmdline}".strip()
        if forensics:
            append += " " + FORENSICS_CMDLINE
        cmd += ["-kernel", kernel, "-initrd", initrd, "-append", append]
    else:
        cmd += ["-cdrom", iso, "-boot", "d"]
        if mode == "uefi":
            cmd += ["-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}"]
    return cmd


def render_summary(results, meta):
    lines = ["# L2 low-RAM boot matrix", ""]
    for k, v in meta.items():
        lines.append(f"- {k}: {v}")
    lines += ["", "| MiB | mode | status | signature/reason | last kts | MemTotal | used | reserved | spawn | log |",
              "|----:|------|--------|------------------|---------:|---------:|-----:|---------:|-------|-----|"]
    for r in results:
        sig = r.get("signature") or r.get("reason") or ""
        ts = r.get("last_ts")
        num = lambda v: "—" if v is None else str(v)
        spawn = {True: "ok", False: "FAIL", None: "—"}[r.get("spawn_ok")]
        log = Path(r["log"]).name if r.get("log") else "—"
        lines.append(f"| {r['size']} | {r['mode']} | {r['status']} | {sig} | "
                     f"{num(ts)} | {num(r.get('mem_total'))} | {num(r.get('used'))} | "
                     f"{num(r.get('reserved'))} | {spawn} | {log} |")
    n = {s: sum(1 for r in results if r["status"] == s) for s in ("PASS", "FAIL", "SKIPPED")}
    lines += ["", f"PASS {n['PASS']} / FAIL {n['FAIL']} / SKIPPED {n['SKIPPED']} "
              f"— exit 0 only if no FAIL.",
              "", "used = MemTotal - MemAvailable (lowest of 3 samples after settle); "
              "reserved = installed - MemTotal.", ""]
    return "\n".join(lines)


# ---------------------------------------------------------------- QEMU guest

class Guest:
    """QEMU with -serial stdio; full log captured incrementally to logfh."""

    def __init__(self, argv, logfh):
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.logfh = logfh
        self.all = bytearray()   # everything ever read (for classification)
        self.buf = b""           # current probe window
        self.dead = False

    def _ingest(self, chunk):
        self.all += chunk
        self.buf += chunk
        self.logfh.write(chunk)
        self.logfh.flush()

    def _drain(self, wait=0.3):
        while True:
            r, _, _ = select.select([self.p.stdout], [], [], wait)
            if not r:
                break
            chunk = os.read(self.p.stdout.fileno(), 65536)
            if not chunk:
                break
            self._ingest(chunk)

    def read_until(self, token, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            r, _, _ = select.select([self.p.stdout], [], [], 0.5)
            if r:
                chunk = os.read(self.p.stdout.fileno(), 65536)
                if not chunk:
                    self.dead = True
                    break
                self._ingest(chunk)
                if token in self.buf:
                    return True
            if self.p.poll() is not None:
                self._drain()
                self.dead = True
                break
        return token in self.buf

    def send(self, line):
        # serial stdio can drop a bare LF: terminate with CRLF, settle first.
        if self.dead:
            return False
        time.sleep(0.7)
        try:
            self.p.stdin.write(line.encode() + b"\r\n")
            self.p.stdin.flush()
            return True
        except OSError:
            self.dead = True
            return False

    def probe(self, cmd, mark, timeout=20):
        self.buf = b""
        if not self.send(probe_line(cmd, mark)):
            return False, ""
        ok = self.read_until(mark.encode() + b"\r", timeout)
        return ok, after_echo(self.buf.decode(errors="replace"), cmd)

    def meminfo(self, samples=3, gap=2.0):
        best, best_used = None, None
        for i in range(samples):
            ok, text = self.probe("cat /proc/meminfo", "L2MEMDONE")
            if not ok:
                break
            info = parse_meminfo(text)
            if "MemTotal" not in info:
                break
            u = used_kib(info)
            if best_used is None or u < best_used:
                best, best_used = info, u
            if i < samples - 1:
                time.sleep(gap)
        return best

    def close(self):
        try:
            if self.p.poll() is None:
                self.p.kill()
        except OSError:
            pass
        try:
            self._drain(0.5)   # capture anything flushed at exit/kill
        except OSError:
            pass
        try:
            self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        for fh in (self.p.stdin, self.p.stdout):
            try:
                fh.close()
            except OSError:
                pass


# ---------------------------------------------------------------- matrix runs

def skipped(size, mode, reason):
    return {"size": size, "mode": mode, "status": "SKIPPED", "signature": None,
            "reason": reason, "last_ts": None, "tail": [], "spawn_ok": None, "log": None}


def run_one(a, size, mode):
    if mode in ("bios", "uefi") and a.forensics:
        return skipped(size, mode, "forensics needs direct boot; ISO cmdline immutable")
    if mode == "uefi" and not a.ovmf:
        return skipped(size, mode, "OVMF firmware not found (pass --ovmf)")
    logpath = Path(a.outdir) / f"{size}M-{mode}.log"
    cmd = build_cmd(size, mode, a.smp, iso=a.iso, kernel=a.kernel, initrd=a.initrd,
                    cmdline=a.cmdline, ovmf=a.ovmf, forensics=a.forensics)
    r = {"size": size, "mode": mode, "status": "FAIL", "signature": None, "reason": None,
         "last_ts": None, "tail": [], "spawn_ok": None, "log": str(logpath),
         "mem_total": None, "mem_available": None, "mem_free": None, "used": None,
         "reserved": None, "slab": None, "pagetables": None, "kernel_stack": None,
         "percpu": None, "kernel_release": None, "cmdline": None, "qemu_exit": None}
    t0 = time.monotonic()
    with open(logpath, "ab") as logfh:   # append: never truncate earlier evidence
        g = Guest(cmd, logfh)
        try:
            if not g.read_until(PROMPT, a.timeout):
                text = bytes(g.all).decode(errors="replace")
                r["signature"] = classify(text)
                r["last_ts"] = last_timestamp(text)
                r["tail"] = log_tail(text)
                r["qemu_exit"] = g.p.poll()
                r["reason"] = "no L2# prompt before timeout" if not g.dead else "QEMU exited before prompt"
                return r
            time.sleep(a.settle)   # let services finish; short settles read high
            info = g.meminfo()
            if not info:
                text = bytes(g.all).decode(errors="replace")
                r["signature"] = classify(text)
                r["last_ts"] = last_timestamp(text)
                r["tail"] = log_tail(text)
                r["reason"] = "meminfo probe failed after prompt"
                return r
            r.update(mem_total=info["MemTotal"], mem_available=info.get("MemAvailable"),
                     mem_free=info.get("MemFree"), used=used_kib(info),
                     reserved=size * 1024 - info["MemTotal"], slab=info.get("Slab"),
                     pagetables=info.get("PageTables"), kernel_stack=info.get("KernelStack"),
                     percpu=info.get("Percpu"))
            ok, text = g.probe("uname -r", "L2VERDONE")
            r["kernel_release"] = first_output_line(text, "L2VERDONE") if ok else None
            ok, text = g.probe("cat /proc/cmdline", "L2CMDDONE")
            r["cmdline"] = first_output_line(text, "L2CMDDONE") if ok else None
            # fork+exec+shell under pressure; the command itself is a broken marker
            # (echoed input contains L2SPAWN''OK, output contains L2SPAWNOK).
            g.buf = b""
            r["spawn_ok"] = g.send("/bin/dash -c 'echo L2SPAWN''OK'") and \
                g.read_until(b"L2SPAWNOK\r", 20)
            r["status"] = "PASS"
            g.send("poweroff")   # best effort; close() kills regardless
            return r
        finally:
            g.close()
            r["seconds"] = round(time.monotonic() - t0, 1)


def progress(r):
    tag = f"[{r['size']}M/{r['mode']}]"
    if r["status"] == "PASS":
        print(f"{tag} PASS used={r['used']} KiB total={r['mem_total']} KiB "
              f"reserved={r['reserved']} KiB spawn={'ok' if r['spawn_ok'] else 'FAIL'} "
              f"({r.get('seconds', '?')}s)", flush=True)
    elif r["status"] == "SKIPPED":
        print(f"{tag} SKIPPED: {r['reason']}", flush=True)
    else:
        print(f"{tag} FAIL {r['signature']} (last kts {r['last_ts']}, "
              f"qemu exit {r['qemu_exit']}, {r.get('seconds', '?')}s) {r['reason']}", flush=True)


def selftest():
    """Run the offline unit checks in-process; no QEMU."""
    import importlib.util
    import unittest
    path = Path(__file__).with_name("test_lowram_matrix.py")
    if path.exists():
        spec = importlib.util.spec_from_file_location("test_lowram_matrix", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        suite = unittest.defaultTestLoader.loadTestsFromModule(mod)
        res = unittest.TextTestRunner(verbosity=1, stream=sys.stderr).run(suite)
        return 0 if res.wasSuccessful() else 1
    assert parse_sizes("10,16M") == [10, 16]
    assert used_kib(parse_meminfo("MemTotal: 1000 kB\nMemAvailable: 300 kB\n")) == 700
    assert classify("Kernel panic - not syncing: System is deadlocked on memory") == "oom-deadlock"
    assert "L2MEMDONE" not in probe_line("cat /proc/meminfo", "L2MEMDONE")
    print("selftest ok (test module missing; ran minimal checks)")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--iso", default="build/out/l2.iso")
    ap.add_argument("--kernel"); ap.add_argument("--initrd")
    ap.add_argument("--sizes", default=DEFAULT_SIZES)
    ap.add_argument("--modes", default="bios,uefi")
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--settle", type=float, default=40.0)
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--outdir")
    ap.add_argument("--ovmf")
    ap.add_argument("--forensics", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    try:
        sizes = parse_sizes(a.sizes)
        modes = resolve_modes(a.modes, bool(a.kernel and a.initrd))
    except ValueError as e:
        ap.error(str(e))
    if bool(a.kernel) != bool(a.initrd):
        ap.error("--kernel and --initrd must be given together")
    if a.smp < 1:
        ap.error("--smp must be >= 1")
    if a.forensics and not (a.kernel and a.initrd):
        ap.error("--forensics requires --kernel/--initrd: the ISO cmdline cannot be "
                 "modified, so forensics only works in direct mode")
    if not a.forensics and any(m in ("bios", "uefi") for m in modes) and not os.path.exists(a.iso):
        ap.error(f"ISO not found: {a.iso}")
    for p in (a.kernel, a.initrd):
        if p and not os.path.exists(p):
            ap.error(f"file not found: {p}")
    if not shutil.which("qemu-system-x86_64"):
        raise SystemExit("qemu-system-x86_64 not found (l2 deps install)")
    a.ovmf = find_ovmf(a.ovmf) if "uefi" in modes else None
    if "uefi" in modes and not a.ovmf:
        print("note: no OVMF firmware found; uefi runs will be SKIPPED (pass --ovmf)", flush=True)
    a.cmdline = ""
    cf = REPO / "config" / "cmdline"
    if cf.exists():
        a.cmdline = cf.read_text().strip()
    else:
        print(f"note: {cf} missing; direct cmdline uses defaults only", flush=True)
    if not a.outdir:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
        a.outdir = str(Path("build/campaigns") / f"lowram-{stamp}")
    Path(a.outdir).mkdir(parents=True, exist_ok=True)
    meta = {"generated (UTC)": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "iso": a.iso, "smp": a.smp, "settle_s": a.settle, "timeout_s": a.timeout,
            "accel": accel()[1], "forensics": a.forensics}
    if a.kernel:
        meta["kernel"] = a.kernel
        meta["initrd"] = a.initrd
    if a.ovmf:
        meta["ovmf"] = a.ovmf
    results, interrupted = [], False
    try:
        for size in sizes:
            for mode in modes:
                r = run_one(a, size, mode)
                results.append(r)
                progress(r)
                (Path(a.outdir) / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    except KeyboardInterrupt:
        interrupted = True
        print("interrupted: QEMU killed, partial results kept", file=sys.stderr, flush=True)
    summary = render_summary(results, meta)
    (Path(a.outdir) / "summary.md").write_text(summary)
    (Path(a.outdir) / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(summary, flush=True)
    if interrupted:
        return 130
    return 1 if any(r["status"] == "FAIL" for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
