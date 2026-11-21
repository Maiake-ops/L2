"""Regression checks for QEMU boot and memory gates without a guest."""
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import ramtest


class Ramtest(unittest.TestCase):
    def setUp(self):
        self.args = SimpleNamespace(iso="l2.iso", disk=None, ram="10", timeout=10,
                                    settle=0, uefi=False, budget=100, sizes="10M")
        self.guest = Mock()
        self.guest.buf = b""
        self.guest.read_until.return_value = True
        self.guest.meminfo.return_value = {"MemTotal": 100, "MemFree": 20}
        self.guest.p.wait.return_value = 0
        for name, value in (("have", lambda _: True), ("Guest", lambda _: self.guest)):
            target = patch.object(ramtest, name, value)
            target.start()
            self.addCleanup(target.stop)

    def test_invalid_sizes_never_start_sweep(self):
        for sizes in ("", "10M,", "0", "10M,bad"):
            self.args.sizes = sizes
            with self.subTest(sizes=sizes), patch.object(ramtest, "measure") as measure:
                with self.assertRaises(SystemExit):
                    ramtest.ram_sweep(self.args)
                measure.assert_not_called()

    def test_memory_accounting(self):
        self.assertEqual(ramtest.used_memory({"MemTotal": 100, "MemFree": 10, "Cached": 60,
                                             "Shmem": 50, "Buffers": 2, "SReclaimable": 3}), 75)
        self.assertEqual(ramtest.used_memory({"MemTotal": 100, "MemAvailable": 70}), 30)

    def test_medium_requires_memory_within_budget(self):
        for info in (None, {"MemTotal": 1000, "MemFree": 0}):
            self.guest.meminfo.return_value = info
            with self.subTest(info=info):
                self.assertEqual(ramtest.boot_medium(self.args), 1)

    def test_old_memory_marker_cannot_complete_probe(self):
        self.guest.buf = b"L2RAMDONE\r"
        self.guest.read_until.side_effect = lambda token, _: token == ramtest.PROMPT or token in self.guest.buf
        self.assertEqual(ramtest.boot_medium(self.args), 1)

    def test_disk_requires_guest_root_evidence(self):
        self.args.iso, self.args.disk = None, "installed.img"
        for root, expected in ((b"rootfs / rootfs rw", 1), (b"/dev/root / ext4 rw", 0)):
            def reply(token, _):
                self.guest.buf = b"7.3.3\r\n" + root + b"\r\nL2RAMDONE\r"
                return True
            self.guest.read_until.side_effect = reply
            self.assertEqual(ramtest.boot_medium(self.args), expected)

    def test_shutdown_failure_is_observable(self):
        self.guest.p.wait.side_effect = ramtest.subprocess.TimeoutExpired("qemu", 10)
        self.assertEqual(ramtest.boot_medium(self.args), 1)
        self.guest.p.wait.side_effect = None
        self.guest.p.wait.return_value = 1
        self.assertEqual(ramtest.boot_medium(self.args), 1)

    def test_ambiguous_media_are_rejected(self):
        for flags in (["--iso", "a", "--disk", "b"], ["--disk", "a", "--kernel", "k"],
                      ["--iso", "a", "--initrd", "i"]):
            with patch("sys.argv", ["ramtest", *flags]), self.assertRaises(SystemExit) as error:
                ramtest.main()
            self.assertEqual(error.exception.code, 2)
