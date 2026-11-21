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

    def test_default_settling_protocol_and_override(self):
        for flags, expected in (([], 40), (["--settle", "2"], 2)):
            with patch("sys.argv", ["ramtest", "--iso", "a", *flags]), patch.object(ramtest, "boot_medium") as boot:
                ramtest.main()
                self.assertEqual(boot.call_args.args[0].settle, expected)

    def test_machine_type_reaches_medium_and_sweep(self):
        self.args.machine = "q35"
        self.args.cmdline = ""
        with patch.object(ramtest, "Guest", return_value=self.guest) as guest:
            ramtest.boot_medium(self.args)
            ramtest.measure(self.args, "256M")
            for call in guest.call_args_list:
                argv = call.args[0]
                self.assertEqual(argv[argv.index("-machine") + 1], "q35")

    def test_release_requires_measured_pid1_rss_within_budget(self):
        self.args.pid1_budget = 50
        for output, expected in ((b"", 1), (b"VmRSS: 0 kB", 1), (b"VmRSS: 52 kB", 1),
                                 (b"VmRSS: 36 kB", 0)):
            def reply(token, _):
                self.guest.buf = b"7.3.3\r\n" + output + b"\r\nL2RAMDONE\r"
                return True
            self.guest.read_until.side_effect = reply
            self.assertEqual(ramtest.boot_medium(self.args), expected)
