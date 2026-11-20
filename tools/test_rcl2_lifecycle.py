"""rcl2 lifecycle + parser fuzz harness (ARMAGEDDON II, workstream W1).

Discover/run:
    python3 -m unittest discover -s tools -p 'test_rcl2_lifecycle.py'

Offline, tmpdir configs, subprocess-driven, stdlib only. NOT wired into
`make check` - the lead integrates. Override the binary under test with
RCL2_BIN=<path> (used to demonstrate failing-before/passing-after against
the pre-fix baseline binary).

Suites:
  ParserFuzz     - >=20 malformed/oversized/boundary configs via --selftest
                   (boundary sizes 2047/2048/2049, NUL bytes, CRLF, token
                   caps from the pinned C/Rust ABI: 16 svcs / 6 args / 8 env,
                   name 24 / path 96 / arg 64 / env 64 / host 64 bytes)
  ElfAudit       - no W+X LOAD segment, non-exec GNU_STACK (link-flag guard)
  Dispatch       - CLI guards, builtin selftest fixtures, RSS + size budgets
  Pid1Lifecycle  - real PID1 in private user/pid/mount/uts namespaces with a
                   private chroot: once/wait ordering, crash-loop backoff
                   timing, zombie burst reaping, SIGTERM shutdown with
                   TERM->KILL escalation, SIGINT ctrl-alt-del, installed
                   mounts-scan smoke. Skips when userns or clang is absent.
"""
import os
from pathlib import Path
import random
import re
import shutil
import signal
import subprocess
import tempfile
import threading
import time
import unittest

REPO = Path(__file__).resolve().parent.parent
RCL2 = Path(os.environ.get("RCL2_BIN") or REPO / "rcl2" / "build" / "rcl2")
HELPER_SRC = REPO / "rcl2" / "verification" / "lifecycle-helper.c"

CFLAGS = ["-target", "x86_64-unknown-linux-gnu", "-c", "-std=c11", "-Os",
          "-Wall", "-Wextra", "-ffreestanding", "-fno-builtin",
          "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
          "-fomit-frame-pointer", "-fno-pic", "-fno-pie", "-fno-ident"]


def ensure_binary():
    if RCL2.exists():
        return
    if os.environ.get("RCL2_BIN"):
        raise unittest.SkipTest(f"RCL2_BIN={RCL2} does not exist")
    r = subprocess.run(["make", "-C", str(REPO / "rcl2")],
                       capture_output=True, text=True, timeout=300)
    if r.returncode != 0 or not RCL2.exists():
        raise unittest.SkipTest("cannot build rcl2 (clang/rustc/ld.lld missing?)")


def run_selftest(conf_bytes, label):
    with tempfile.TemporaryDirectory(prefix="rcl2-fuzz-") as td:
        p = Path(td) / label
        p.write_bytes(conf_bytes)
        r = subprocess.run([str(RCL2), "--selftest", str(p)],
                           capture_output=True, text=True, timeout=30)
    return r.returncode, r.stdout + r.stderr


def accepted(out):
    m = re.search(r"config (\d+) service\(s\) accepted", out)
    return int(m.group(1)) if m else None


def filler(n):
    """exactly n bytes of comment lines (n % 40 must not leave 1 byte)"""
    assert n % 40 != 1
    out = bytearray()
    while n - len(out) >= 40:
        out += b"#" + b"a" * 38 + b"\n"
    rest = n - len(out)
    if rest:
        out += b"#" + b"a" * (rest - 2) + b"\n"
    return bytes(out)


class FuzzBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        ensure_binary()

    def check_case(self, name, data, rc=0, nsvc=None, contains=(), absent=()):
        code, out = run_selftest(data, name)
        self.assertNotIn("PANIC", out, f"{name}: rust panic")
        self.assertEqual(code, rc, f"{name}: exit {code}\n{out}")
        if nsvc is not None:
            self.assertEqual(accepted(out), nsvc, f"{name}: nsvc\n{out}")
        for s in contains:
            self.assertIn(s, out, f"{name}: missing {s!r}\n{out}")
        for s in absent:
            self.assertNotIn(s, out, f"{name}: unexpected {s!r}\n{out}")
        return out


class ParserFuzz(FuzzBase):
    def test_boundary_sizes(self):
        svc = b"service ok once /bin/true\n"          # 26 bytes
        good2047 = filler(2047 - len(svc)) + svc
        self.check_case("exact2047", good2047, nsvc=1)
        self.check_case("comments2047", filler(2047), nsvc=0)
        self.check_case("over2048", good2047 + b"\n", rc=1,
                        contains=("config too large",))
        self.check_case("over2049", good2047 + b"\n\n", rc=1,
                        contains=("config too large",))
        self.check_case("empty", b"", nsvc=0)
        self.check_case("one_byte", b"\n", nsvc=0)

    def test_malformed_configs(self):
        self.check_case("comments_only", b"# hi\n\n# bye\n", nsvc=0)
        self.check_case("no_trailing_newline", b"service a once /bin/true",
                        nsvc=1)
        self.check_case("crlf", b"service a once /bin/true\r\nhostname C\r\n",
                        nsvc=1, contains=("hostname=C",))
        self.check_case("tabs", b"service\ta\tonce\t/bin/true\n", nsvc=1)
        self.check_case("inline_comment",
                        b"service a once /bin/true # trailing\n", nsvc=1)
        self.check_case("nul_midpath",
                        b"service a respawn /bin/true\x00evil\n"
                        b"service b once /bin/false\n",
                        nsvc=1, contains=("[b]",), absent=("[a]",))
        self.check_case("nul_tail_line",
                        b"hostname X\nservice a once /bin/true\n\x00\n",
                        nsvc=1, contains=("hostname=X",))
        self.check_case("bare_directive", b"service\nhostname\n", nsvc=0)
        self.check_case("bad_mode", b"service a sometimes /bin/true\n", nsvc=0)
        self.check_case("unknown_directives",
                        b"bogus x y\nservice a once /bin/true\nmore junk\n",
                        nsvc=1)
        self.check_case("overlong_token", b"service a once /" + b"p" * 2000,
                        nsvc=0)
        rng = random.Random(42)
        self.check_case("binary_garbage", rng.randbytes(1500))
        self.check_case("binary_garbage_2", rng.randbytes(2047))

    def test_token_limits(self):
        # path cap 96: 95 chars fit, 96 skip the line (never truncate)
        self.check_case("path95", b"service a once /" + b"p" * 94 + b"\n",
                        nsvc=1)
        self.check_case("path96", b"service a once /" + b"p" * 95 + b"\n",
                        nsvc=0)
        # arg cap 64: 63 chars fit, 64 skip the line
        head = b"service a respawn /bin/dash -c "
        self.check_case("arg63", head + b"q" * 63 + b"\n", nsvc=1)
        self.check_case("arg64", head + b"q" * 64 + b"\n", nsvc=0)
        # 6 args fit, a 7th skips the line (never silently dropped)
        six = b"service a respawn /bin/dash" + b" a" * 6 + b"\n"
        self.check_case("args6", six, nsvc=1)
        self.check_case("args7", six[:-1] + b" a\n", nsvc=0)
        # 16 services fill the table, the 17th is skipped
        line = b"service s respawn /bin/true\n"
        self.check_case("svcs16", line * 16, nsvc=16)
        self.check_case("svcs17", line * 17, nsvc=16)
        # env cap 64 bytes per pair, 8 slots
        self.check_case("env63", b"setenv K=" + b"v" * 61 + b"\n",
                        contains=("nenv=1",))
        self.check_case("env64", b"setenv K=" + b"v" * 62 + b"\n",
                        contains=("nenv=0",))
        self.check_case("env9", b"setenv K=v\n" * 9, contains=("nenv=8",))
        self.check_case("env_no_value", b"setenv K=\n", contains=("nenv=0",))
        # klog sanity: 0..7, >255 or non-numeric leaves it unchanged
        self.check_case("klog255", b"klog 255\n", contains=("klog=7",))
        self.check_case("klog256", b"klog 256\n", contains=("klog=0",))
        self.check_case("klogabc", b"klog abc\n", contains=("klog=0",))
        # hostname truncates at 63 (label-only field, documented limitation)
        self.check_case("host63", b"hostname " + b"h" * 63 + b"\n",
                        contains=("hostname=" + "h" * 63,))
        self.check_case("host64", b"hostname " + b"h" * 64 + b"\n",
                        contains=("hostname=" + "h" * 63 + " ",))

    def test_max_load(self):
        # 16 services x 6 args + 8 envs + hostname, all inside 2047 bytes
        conf = b"hostname MAX\nklog 7\nctrlaltdel poweroff\n"
        conf += b"setenv E=v\n" * 8
        conf += (b"service s respawn /bin/true a b c d e f\n" * 16)
        self.assertLessEqual(len(conf), 2047)
        out = self.check_case("maxload", conf, nsvc=16,
                              contains=("nenv=8", "klog=7", "cad=1"))
        self.assertEqual(out.count("nargs=6"), 16)


class ElfAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        ensure_binary()
        cls.readelf = shutil.which("readelf") or shutil.which("llvm-readelf")
        if not cls.readelf:
            raise unittest.SkipTest("readelf/llvm-readelf not available")

    def test_no_wx_segment_and_nonexec_stack(self):
        out = subprocess.run([self.readelf, "-lW", str(RCL2)],
                             capture_output=True, text=True, timeout=30).stdout
        loads = re.findall(r"^\s*LOAD\s+(?:\S+\s+){5}(.+?)\s+\S+\s*$",
                           out, re.M)
        self.assertTrue(loads, f"no LOAD segments parsed:\n{out}")
        for flg in loads:
            self.assertFalse("W" in flg and "E" in flg,
                             f"W+X LOAD segment: {flg!r}")
        stack = re.findall(r"^\s*GNU_STACK\s+(?:\S+\s+){5}(.+?)\s+\S+\s*$",
                           out, re.M)
        self.assertTrue(stack, "GNU_STACK header missing (exec stack risk)")
        for flg in stack:
            self.assertNotIn("E", flg, f"executable stack: {flg!r}")


class Dispatch(FuzzBase):
    def test_not_pid1_refused(self):
        r = subprocess.run([str(RCL2)], capture_output=True, text=True,
                           timeout=30)
        self.assertEqual(r.returncode, 1)
        self.assertIn("not running as pid 1", r.stdout + r.stderr)

    def test_builtin_selftest(self):
        r = subprocess.run([str(RCL2), "--selftest"], capture_output=True,
                           text=True, timeout=30)
        out = r.stdout + r.stderr
        self.assertEqual(r.returncode, 0, out)
        self.assertIn("selftest ok", out)
        self.assertIn("3 service(s) accepted", out)
        # cmdline fixtures: trailing-\n cases 3/4 pin the rescue-flag fix
        for t, flags in ((0, 1), (1, 1), (2, 0), (3, 1), (4, 1)):
            self.assertIn(f"cmdline test {t} flags={flags}", out)
        m = re.search(r"resident (\d+) KiB", out)
        self.assertTrue(m, out)
        rss = int(m.group(1))
        self.assertLessEqual(rss, 60, f"selftest RSS {rss} KiB (target ~50)")
        self.assertLessEqual(rss, 150)

    def test_binary_size_budget(self):
        size = RCL2.stat().st_size
        # baseline 17184 B; hard guard against material growth
        self.assertLessEqual(size, 20480, f"binary grew to {size} B")


class Pid1Lifecycle(unittest.TestCase):
    """Real PID1 boot in private namespaces + chroot (no host side effects).

    rcl2 runs as PID 1 of a fresh pid namespace owned by a fresh user
    namespace; mounts land in a private mount namespace. The service binary
    is the freestanding lifecycle-helper (raw syscalls, no libc). Cleanup
    kills only our own PIDs - never a broad process-name kill.
    """

    @classmethod
    def setUpClass(cls):
        ensure_binary()
        for tool in ("unshare", "clang", "ld.lld"):
            if not shutil.which(tool):
                raise unittest.SkipTest(f"{tool} not available")
        probe = subprocess.run(["unshare", "-Urpf", "true"],
                               capture_output=True, timeout=15)
        if probe.returncode != 0:
            raise unittest.SkipTest("unprivileged userns/pidns not available")
        cls._tmp = tempfile.TemporaryDirectory(prefix="rcl2-lifecycle-")
        cls.addClassCleanup(cls._tmp.cleanup)
        cls.helper = Path(cls._tmp.name) / "helper"
        obj = Path(cls._tmp.name) / "helper.o"
        r = subprocess.run(["clang", *CFLAGS, str(HELPER_SRC), "-o", str(obj)],
                           capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            raise unittest.SkipTest(f"helper compile failed: {r.stderr}")
        r = subprocess.run(["ld.lld", "-static", "-o", str(cls.helper), str(obj)],
                           capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            raise unittest.SkipTest(f"helper link failed: {r.stderr}")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="rcl2-root-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for d in ("bin", "etc", "proc", "sys", "dev", "root"):
            (self.root / d).mkdir()
        shutil.copy2(RCL2, self.root / "bin" / "rcl2")
        shutil.copy2(self.helper, self.root / "bin" / "helper")
        self.lines = []          # (monotonic_ts, text)
        self.proc = None
        self.addCleanup(self._cleanup)

    # ------------------------------------------------------------ plumbing
    def _reader(self):
        for raw in iter(self.proc.stdout.readline, b""):
            self.lines.append((time.monotonic(),
                               raw.decode(errors="replace").rstrip("\r\n")))

    def start(self, conf, installed=False):
        (self.root / "etc" / "rcl2.conf").write_text(conf)
        if installed:
            (self.root / "etc" / "l2").mkdir()
            (self.root / "etc" / "l2" / "installed").write_text("1\n")
        self.proc = subprocess.Popen(
            ["unshare", "--user", "--map-root-user", "--pid", "--mount",
             "--uts", "--fork", "chroot", str(self.root), "/bin/rcl2"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        threading.Thread(target=self._reader, daemon=True).start()

    def wait_log(self, pattern, timeout=15):
        end = time.monotonic() + timeout
        rx = re.compile(pattern)
        while time.monotonic() < end:
            for _, line in self.lines:
                if rx.search(line):
                    return line
            if self.proc.poll() is not None:
                break
            time.sleep(0.05)
        tail = "\n".join(l for _, l in self.lines[-25:])
        self.fail(f"timeout waiting for {pattern!r}; log tail:\n{tail}")

    def stamps(self, pattern):
        rx = re.compile(pattern)
        return [ts for ts, line in self.lines if rx.search(line)]

    def count_log(self, pattern):
        rx = re.compile(pattern)
        return sum(1 for _, line in self.lines if rx.search(line))

    def rcl2_pid(self):
        """host pid of the namespaced PID1 (unshare -> chroot -> rcl2)"""
        end = time.monotonic() + 10
        while time.monotonic() < end:
            try:
                kids = Path(f"/proc/{self.proc.pid}/task/{self.proc.pid}"
                            f"/children").read_text().split()
                if kids:
                    return int(kids[0])
            except OSError:
                pass
            time.sleep(0.05)
        self.fail("could not locate namespaced rcl2 pid")

    def child_pids(self):
        pid = self.rcl2_pid()
        try:
            return [int(x) for x in
                    Path(f"/proc/{pid}/task/{pid}/children").read_text().split()]
        except OSError:
            return []

    def _cleanup(self):
        if self.proc is None:
            return
        try:
            pid = self.rcl2_pid() if self.proc.poll() is None else None
        except AssertionError:
            pid = None
        for target in (pid, self.proc.pid):
            if target:
                try:
                    os.kill(target, signal.SIGKILL)
                except OSError:
                    pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        if self.proc.stdout:
            self.proc.stdout.close()

    # --------------------------------------------------------------- tests
    def test_once_wait_ordering(self):
        self.start("hostname NS\nklog 0\n"
                   "service o once /bin/helper exit 3\n"
                   "service w wait /bin/helper sleep 200\n"
                   "service r respawn /bin/helper sleep 60000\n")
        self.wait_log(r"boot complete: 3 service\(s\)")
        self.wait_log(r"o done \(exit 3\)")
        self.wait_log(r"w finished \(exit 0\)")
        o = self.stamps(r"o done")[0]
        w = self.stamps(r"w finished")[0]
        self.assertLess(o, w, "once exit must be logged before wait finishes")
        os.kill(self.rcl2_pid(), signal.SIGTERM)
        self.wait_log(r"shutting down: poweroff request")

    def test_crashloop_backoff_timing(self):
        self.start("hostname NS\nklog 0\n"
                   "service c respawn /bin/helper exit 7\n")
        self.wait_log(r"retry in 2s", timeout=15)
        # the third spawn follows the 2s backoff logged above
        end = time.monotonic() + 10
        while len(self.stamps(r"start c pid")) < 3 and time.monotonic() < end:
            time.sleep(0.05)
        starts = self.stamps(r"start c pid")
        self.assertGreaterEqual(len(starts), 3, "expected >=3 spawns")
        d1, d2 = starts[1] - starts[0], starts[2] - starts[1]
        self.assertTrue(0.7 <= d1 <= 1.9, f"first backoff {d1:.2f}s != ~1s")
        self.assertTrue(1.5 <= d2 <= 3.2, f"second backoff {d2:.2f}s != ~2s")
        self.wait_log(r"c exited \(exit 7\), retry in 1s")

    def test_zombie_burst_reaping(self):
        self.start("hostname NS\nklog 0\n"
                   "service b once /bin/helper burst 25\n"
                   "service keep respawn /bin/helper sleep 60000\n")
        self.wait_log(r"b done \(exit 0\)")
        end = time.monotonic() + 10
        while self.count_log(r"rcl2: reap ") < 25 and time.monotonic() < end:
            time.sleep(0.05)
        self.assertGreaterEqual(self.count_log(r"rcl2: reap "), 25,
                                "burst zombies not fully reaped")
        self.assertEqual(len(self.child_pids()), 1,
                         "only the long-lived service may remain")

    def test_sigterm_kill_escalation(self):
        self.start("hostname NS\nklog 0\n"
                   "service s respawn /bin/helper stubborn\n")
        self.wait_log(r"start s pid")
        kids = self.child_pids()
        self.assertEqual(len(kids), 1)
        os.kill(self.rcl2_pid(), signal.SIGTERM)
        self.wait_log(r"shutting down: poweroff request")
        # stubborn ignores TERM: it must die via the KILL sweep (~5s grace)
        end = time.monotonic() + 12
        while Path(f"/proc/{kids[0]}").exists() and time.monotonic() < end:
            time.sleep(0.1)
        self.assertFalse(Path(f"/proc/{kids[0]}").exists(),
                         "TERM-immune service survived shutdown (no KILL?)")

    def test_sigint_ctrlaltdel(self):
        self.start("hostname NS\nklog 0\nctrlaltdel poweroff\n"
                   "service r respawn /bin/helper sleep 60000\n")
        self.wait_log(r"boot complete")
        os.kill(self.rcl2_pid(), signal.SIGINT)
        self.wait_log(r"shutting down: ctrl-alt-del")

    def test_installed_mounts_scan_smoke(self):
        # installed mark exercises the streaming /proc/mounts ro guard
        # against real kernel output; host root is rw -> no remount log
        self.start("hostname NS\nklog 0\n"
                   "service r respawn /bin/helper sleep 60000\n",
                   installed=True)
        self.wait_log(r"boot complete")
        text = "\n".join(l for _, l in self.lines)
        self.assertNotIn("PANIC", text)
        self.assertNotIn("root was read-only", text)


if __name__ == "__main__":
    unittest.main()
