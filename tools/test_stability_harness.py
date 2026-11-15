"""Regression checks for serial framing, memory accounting, and result gates."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("stability", Path(__file__).with_name("test-stability.py"))
stability = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stability)


class AccountingTests(unittest.TestCase):
    def test_available_instead_of_free(self):
        info = stability.meminfo("MemTotal: 10000 kB\nMemFree: 100 kB\n"
                                 "MemAvailable: 9000 kB\nCached: 8000 kB\n")
        self.assertEqual(stability.used_memory(info), 1000)

    def test_fallback_does_not_treat_tmpfs_as_reclaimable(self):
        info = {"MemTotal": 10000, "MemFree": 1000, "Cached": 4000,
                "Buffers": 100, "SReclaimable": 200, "Shmem": 3000}
        self.assertEqual(stability.used_memory(info), 7700)

    def test_missing_memory_fails(self):
        with self.assertRaises(stability.Failure):
            stability.used_memory({"MemFree": 1})


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.args = SimpleNamespace(output=Path(self.temp.name), boots=100, soak_hours=72)
        self.report = {"matrix": [self.row(16, False, "EXPLORATORY_FAILURE"),
                                  self.row(32, True, "PASS")],
                       "errors": [], "iso_budget_passed": True,
                       "filesystem": {"status": "PASS"}}

    @staticmethod
    def row(ram, required, status):
        return {"ram_mib": ram, "required": required, "status": status,
                "boots_passed": 100 if status == "PASS" else 0,
                "shell_reached": status == "PASS"}

    def save(self):
        stability.write_report(self.args, self.report, [])
        return json.loads((self.args.output / "report.json").read_text())

    def test_exploratory_failure_does_not_fail_supported_matrix(self):
        result = self.save()
        self.assertTrue(result["passed"])
        self.assertTrue(result["release_gates_satisfied"])
        self.assertEqual(result["minimum_observed_boot_ram_mib"], 32)

    def test_required_failure_and_interruption_cannot_pass(self):
        for status in ("FAIL", "INCOMPLETE"):
            with self.subTest(status=status):
                self.report["matrix"][1]["status"] = status
                self.assertFalse(self.save()["passed"])

    def test_short_soak_and_skipped_filesystem_are_not_release_passes(self):
        self.args.soak_hours = 24
        self.assertFalse(self.save()["release_gates_satisfied"])
        self.args.soak_hours = 72
        self.report["filesystem"]["status"] = "SKIP"
        self.assertFalse(self.save()["release_gates_satisfied"])

    def test_iso_budget_and_filesystem_failures_propagate(self):
        self.report["iso_budget_passed"] = False
        self.assertFalse(self.save()["passed"])
        self.report["iso_budget_passed"] = True
        self.report["filesystem"]["status"] = "FAIL"
        self.assertFalse(self.save()["passed"])

    def test_run_error_prevents_success_even_with_passing_rows(self):
        self.report["errors"].append("interrupted")
        self.assertFalse(self.save()["passed"])


class SerialTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        fake = self.root / "fake-qemu"
        fake.write_text(f"#!{sys.executable}\n" + r'''
import re, sys, time
for line in sys.stdin:
    print(line.rstrip(), flush=True)  # realistic terminal command echo
    match = re.search(r"L2CHECK_ ([0-9a-f]{32})", line)
    if not match or "no-marker" in line:
        continue
    if "guest-fault" in line:
        print("Kernel panic - not syncing", flush=True)
    print("sample output", flush=True)
    sys.stdout.write("\nL2CHECK_" + match.group(1) + ":")
    sys.stdout.flush()
    time.sleep(0.02)  # marker and numeric status arrive separately
    print("7" if "nonzero" in line else "0", flush=True)
''')
        fake.chmod(0o755)
        self.args = SimpleNamespace(qemu=str(fake), cpus=2, accel="tcg", iso=self.root / "dummy.iso",
                                    command_timeout=0.3)

    def guest(self):
        guest = stability.Guest(self.args, self.root / "vm", 256)
        self.addCleanup(guest.close)
        return guest

    def test_split_status_and_echo_are_handled(self):
        output = self.guest().command("echo hello")
        self.assertIn("sample output", output)
        self.assertIn("L2CHECK_ ", output)

    def test_nonzero_status_is_failure(self):
        with self.assertRaises(stability.CommandFailure):
            self.guest().command("nonzero")

    def test_echo_alone_cannot_complete_command(self):
        with self.assertRaisesRegex(stability.Failure, "serial timeout"):
            self.guest().command("no-marker")

    def test_kernel_fault_prevents_pass(self):
        with self.assertRaisesRegex(stability.Failure, "guest fault"):
            self.guest().command("guest-fault")


class ArgumentsTests(unittest.TestCase):
    def test_invalid_matrix_and_nonfinite_soak_are_rejected(self):
        for argv in (["--ram", "0"], ["--ram", "16", "--min-ram", "32"],
                     ["--soak-hours", "nan"], ["--soak-hours", "inf"]):
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    stability.parse_args(argv)
                self.assertEqual(error.exception.code, 2)


class FilesystemTests(unittest.TestCase):
    def run_scenario(self, corrupt=False):
        with tempfile.TemporaryDirectory() as temporary:
            args = SimpleNamespace(output=Path(temporary), fs_ram=256, command_timeout=1)
            report = {}
            events = []
            digest = "a" * 64

            class FakeGuest:
                active = False

                def __init__(self, args, directory, ram, disk):
                    self.phase = directory.name
                    self.process = self
                    self.active = True
                    FakeGuest.active = True
                    events.append(("start", self.phase))

                def boot(self):
                    pass

                def command(self, script):
                    if script == "cat /mnt/l2-stability/committed.sha256":
                        return digest + "  committed\n"
                    if "sha256sum committed" in script:
                        if corrupt and self.phase == "fs-recovery":
                            raise stability.CommandFailure("committed hash mismatch")
                        return digest + "  committed\n"
                    return ""

                def shutdown(self, reboot=False):
                    events.append(("shutdown", self.phase))
                    FakeGuest.active = False

                def send(self, script):
                    pass

                def wait(self, *args):
                    pass

                def pause(self, seconds):
                    pass

                def kill(self):
                    events.append(("power-loss", self.phase))

                def close(self):
                    self.active = False
                    FakeGuest.active = False
                    events.append(("close", self.phase))

            def offline_check(args, disk, phase, repair=False):
                self.assertFalse(FakeGuest.active, "fsck must never run while QEMU owns the disk")
                self.assertTrue(disk.is_file())
                self.assertEqual(disk.stat().st_size, 64 * 1024 * 1024)
                events.append(("repair" if repair else "fsck", phase))
                return 1 if repair else 0

            with patch.object(stability, "Guest", FakeGuest), \
                    patch.object(stability, "fsck", offline_check), \
                    patch.object(stability.subprocess, "run"), \
                    contextlib.redirect_stdout(io.StringIO()):
                stability.filesystem(args, report)
            return report, events

    def test_reboot_and_crash_recovery_order_and_offline_fsck(self):
        report, events = self.run_scenario()
        self.assertEqual(report["filesystem"]["status"], "PASS")
        self.assertEqual(report["filesystem"]["repair_exit_code"], 1)
        self.assertLess(events.index(("power-loss", "fs-clean-reboot")),
                        events.index(("repair", "crash-repair")))
        self.assertLess(events.index(("repair", "crash-repair")),
                        events.index(("start", "fs-recovery")))

    def test_corruption_after_recovery_fails_and_closes_vm(self):
        report, events = self.run_scenario(corrupt=True)
        self.assertEqual(report["filesystem"]["status"], "FAIL")
        self.assertIn("committed hash mismatch", report["filesystem"]["errors"][0])
        self.assertEqual(events[-1], ("close", "fs-recovery"))


if __name__ == "__main__":
    unittest.main()
