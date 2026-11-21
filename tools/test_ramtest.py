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
