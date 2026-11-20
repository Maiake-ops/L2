"""Offline regression checks for lowram_matrix: parsing, classification,
serial echo framing, and summary rendering.  No QEMU, no network, fast.
Run: python3 -m unittest discover -s tools -p 'test_*.py'
"""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("lowram_matrix", Path(__file__).with_name("lowram_matrix.py"))
lm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lm)

MEMINFO_FIXTURE = """MemTotal:        8192 kB
MemFree:         1234 kB
MemAvailable:    2656 kB
Buffers:           12 kB
Cached:          1200 kB
Shmem:            280 kB
Slab:             900 kB
SReclaimable:     200 kB
SUnreclaim:       700 kB
KernelStack:      272 kB
PageTables:       280 kB
Percpu:           128 kB
"""

# Realistic 12M failure tail from build/campaigns/baseline-ram-sweep.log.
OOM_12M_LOG = """[    0.395166] Node 0 active_anon:20kB inactive_anon:200kB shmem:280kB kernel_stack:272kB pagetables:280kB
[    0.395171] DMA32 free:120kB min:128kB low:160kB high:192kB present:11768kB managed:3212kB
[    0.395191] 2942 pages RAM
[    0.395193] 2139 pages reserved
[    0.395196] Out of memory and no killable processes...
[    0.395198] Kernel panic - not syncing: System is deadlocked on memory
[    0.405006] ---[ end Kernel panic - not syncing: System is deadlocked on memory ]---
"""


class SizeParsing(unittest.TestCase):
    def test_default_list(self):
        self.assertEqual(lm.parse_sizes(lm.DEFAULT_SIZES),
                         [10, 12, 14, 16, 20, 24, 32, 64, 128, 256])

    def test_whitespace_and_suffix(self):
        self.assertEqual(lm.parse_sizes(" 10 , 12,16M "), [10, 12, 16])
        self.assertEqual(lm.parse_sizes("10,,12,"), [10, 12])  # empty tokens ignored

    def test_rejects_bad(self):
        for bad in ("0", "-4", "abc", "1.5", "", "1G"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                lm.parse_sizes(bad)


class ModeParsing(unittest.TestCase):
    def test_default_and_dedupe(self):
        self.assertEqual(lm.resolve_modes("bios,uefi,bios", False), ["bios", "uefi"])

    def test_direct_auto_added(self):
        self.assertEqual(lm.resolve_modes("bios,uefi", True), ["bios", "uefi", "direct"])

    def test_direct_explicit_needs_files(self):
        with self.assertRaises(ValueError):
            lm.resolve_modes("direct", False)
        self.assertEqual(lm.resolve_modes("direct", True), ["direct"])

    def test_rejects_bad(self):
        with self.assertRaises(ValueError):
            lm.resolve_modes("bios,pcboot", False)


class MeminfoParsing(unittest.TestCase):
    def test_fixture_fields(self):
        info = lm.parse_meminfo(MEMINFO_FIXTURE)
        self.assertEqual(info["MemTotal"], 8192)
        self.assertEqual(info["MemAvailable"], 2656)
        self.assertEqual(info["Slab"], 900)
        self.assertEqual(info["PageTables"], 280)
        self.assertEqual(info["KernelStack"], 272)
        self.assertEqual(info["Percpu"], 128)

    def test_used_matches_baseline(self):
        self.assertEqual(lm.used_kib(lm.parse_meminfo(MEMINFO_FIXTURE)), 5536)

    def test_available_fallback(self):
        info = lm.parse_meminfo("MemTotal: 10000 kB\nMemFree: 1000 kB\nCached: 4000 kB\n")
        self.assertEqual(lm.used_kib(info), 5000)

    def test_missing_total_raises(self):
        with self.assertRaises(ValueError):
            lm.used_kib({"MemFree": 1})

    def test_junk_ignored(self):
        self.assertEqual(lm.parse_meminfo("garbage\nL2MEMDONE\n"), {})


class Classification(unittest.TestCase):
    def test_no_cpio_magic(self):
        log = "[    0.210123] Initramfs unpacking failed: no cpio magic\n"
        self.assertEqual(lm.classify(log), "no-cpio-magic")

    def test_oom_deadlock_12m(self):
        self.assertEqual(lm.classify(OOM_12M_LOG), "oom-deadlock")

    def test_oom_kill(self):
        log = "[    1.000000] Out of memory: Killed process 42 (dash) total-vm:100kB\n"
        self.assertEqual(lm.classify(log), "oom-kill")
        self.assertEqual(lm.classify("some long log prefix oom-kill: constraint=0"), "oom-kill")

    def test_decompress_fail(self):
        for s in ("decompressor failed", "invalid magic at start of compressed archive",
                  "Failed to execute /init (error -2)"):
            with self.subTest(s=s):
                self.assertEqual(lm.classify(f"[    0.500000] {s}"), "decompress-fail")

    def test_no_init(self):
        for s in ("No working init found", "Attempted to kill init!", "Requested init /init failed"):
            with self.subTest(s=s):
                self.assertEqual(lm.classify(f"[    0.600000] {s}"), "no-init")

    def test_panic_other(self):
        log = "[    0.700000] Kernel panic - not syncing: VFS: Unable to mount root fs\n"
        self.assertEqual(lm.classify(log), "panic-other")

    def test_silent(self):
        self.assertEqual(lm.classify(""), "silent")
        self.assertEqual(lm.classify("tiny"), "silent")

    def test_timeout_no_prompt(self):
        log = "Limine 8.x booting L2\n[    0.000000] Linux version 7.3.3\n" + "boot line\n" * 10
        self.assertEqual(lm.classify(log), "timeout-no-prompt")

    def test_case_insensitive(self):
        self.assertEqual(lm.classify("KERNEL PANIC - NOT SYNCING: SYSTEM IS DEADLOCKED ON MEMORY"),
                         "oom-deadlock")

    def test_last_timestamp(self):
        self.assertEqual(lm.last_timestamp(OOM_12M_LOG), 0.405006)
        self.assertIsNone(lm.last_timestamp("no timestamps here"))

    def test_tail(self):
        log = "\n".join(f"line {i}" for i in range(30))
        tail = lm.log_tail(log)
        self.assertEqual(len(tail), 15)
        self.assertEqual(tail[-1], "line 29")
        self.assertEqual(tail[0], "line 15")


class BrokenMarker(unittest.TestCase):
    def test_echo_never_contains_marker(self):
        line = lm.probe_line("cat /proc/meminfo", "L2MEMDONE")
        self.assertNotIn("L2MEMDONE", line)          # echoed input cannot fake completion
        self.assertEqual(lm.split_marker("L2MEMDONE"), "L2ME''MDONE")
        self.assertIn("L2ME''MDONE", line)
        self.assertIn("L2MEMDONE", line.replace("''", ""))  # shell concatenation yields it

    def test_completion_token_not_in_echo(self):
        line = lm.probe_line("uname -r", "L2VERDONE")
        self.assertNotIn(b"L2VERDONE\r", line.encode())

    def test_spawn_probe_echo_safe(self):
        cmd = "/bin/dash -c 'echo L2SPAWN''OK'"
        self.assertNotIn(b"L2SPAWNOK", cmd.encode())
        self.assertIn(b"L2SPAWNOK", cmd.replace("''", "").encode())

    def test_after_echo_splits_on_command(self):
        echoed = "L2# cat /proc/meminfo; echo L2ME''MDONE"
        capture = (echoed + "\r\nMemTotal:        8192 kB\r\nMemAvailable:    2656 kB\r\n"
                   "L2MEMDONE\r\nL2# ")
        self.assertNotIn("L2MEMDONE", echoed)        # echoed line cannot fake completion
        self.assertEqual(capture.count("L2MEMDONE\r"), 1)  # only the real output matches
        text = lm.after_echo(capture, "cat /proc/meminfo")
        info = lm.parse_meminfo(text)
        self.assertEqual(lm.used_kib(info), 5536)

    def test_after_echo_missing_command(self):
        self.assertEqual(lm.after_echo("raw output", "cat /proc/meminfo"), "raw output")

    def test_first_output_line_skips_echo_remnants(self):
        text = "; echo L2VE''RDONE\r\n7.3.3-l2\r\nL2VERDONE\r\nL2# "
        self.assertEqual(lm.first_output_line(text, "L2VERDONE"), "7.3.3-l2")


class CommandBuilding(unittest.TestCase):
    def setUp(self):
        patcher = patch.object(lm, "accel", return_value=["-accel", "kvm"])
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_bios(self):
        cmd = lm.build_cmd(16, "bios", 2, iso="l2.iso")
        self.assertIn("-m", cmd)
        self.assertEqual(cmd[cmd.index("-m") + 1], "16M")
        self.assertEqual(cmd[cmd.index("-smp") + 1], "2")
        self.assertIn("-cdrom", cmd)
        self.assertIn("-no-reboot", cmd)
        self.assertIn("-nic", cmd)
        self.assertNotIn("-kernel", cmd)

    def test_uefi_pflash(self):
        cmd = lm.build_cmd(12, "uefi", 2, iso="l2.iso", ovmf="/x/OVMF_CODE.fd")
        self.assertIn("if=pflash,format=raw,readonly=on,file=/x/OVMF_CODE.fd", cmd)

    def test_direct_cmdline(self):
        cmd = lm.build_cmd(10, "direct", 2, kernel="K", initrd="I", cmdline="loglevel=4")
        append = cmd[cmd.index("-append") + 1]
        self.assertTrue(append.startswith("rdinit=/init console=ttyS0,115200 panic=-1"))
        self.assertIn("loglevel=4", append)
        self.assertNotIn("memblock=debug", append)

    def test_direct_forensics_appends(self):
        cmd = lm.build_cmd(10, "direct", 2, kernel="K", initrd="I", forensics=True)
        append = cmd[cmd.index("-append") + 1]
        self.assertIn(lm.FORENSICS_CMDLINE, append)

    def test_ovmf_search_order(self):
        self.assertEqual(lm.find_ovmf("/explicit.fd"), "/explicit.fd")
        with patch.object(lm.os.path, "exists", side_effect=lambda p: p == lm.OVMF_PATHS[0]):
            self.assertEqual(lm.find_ovmf(None), lm.OVMF_PATHS[0])
        with patch.object(lm.os.path, "exists", return_value=False):
            self.assertIsNone(lm.find_ovmf(None))


class SummaryRendering(unittest.TestCase):
    RESULTS = [
        {"size": 10, "mode": "bios", "status": "FAIL", "signature": "oom-deadlock",
         "reason": "QEMU exited before prompt", "last_ts": 0.405006, "spawn_ok": None,
         "log": "/tmp/10M-bios.log", "mem_total": None, "used": None, "reserved": None},
        {"size": 16, "mode": "bios", "status": "PASS", "signature": None, "reason": None,
         "last_ts": None, "spawn_ok": True, "log": "/tmp/16M-bios.log",
         "mem_total": 8192, "used": 5536, "reserved": 8192},
        {"size": 16, "mode": "uefi", "status": "SKIPPED", "signature": None,
         "reason": "OVMF firmware not found (pass --ovmf)", "last_ts": None,
         "spawn_ok": None, "log": None},
    ]

    def test_mixed_table(self):
        md = lm.render_summary(self.RESULTS, {"iso": "l2.iso", "smp": 2})
        self.assertIn("| 10 | bios | FAIL | oom-deadlock |", md)
        self.assertIn("| 16 | bios | PASS |", md)
        self.assertIn("5536", md)
        self.assertIn("| 16 | uefi | SKIPPED | OVMF firmware not found", md)
        self.assertIn("PASS 1 / FAIL 1 / SKIPPED 1", md)
        self.assertIn("16M-bios.log", md)
        self.assertIn("0.405006", md)

    def test_empty_results(self):
        md = lm.render_summary([], {})
        self.assertIn("PASS 0 / FAIL 0 / SKIPPED 0", md)


if __name__ == "__main__":
    unittest.main()
