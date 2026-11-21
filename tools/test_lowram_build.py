"""Regression checks for the VM image builder in private output directories."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LowramBuild(unittest.TestCase):
    @unittest.skipUnless((ROOT / "build/out/limine/bin/limine").is_file() and
                         (ROOT / "build-lowram/out/initramfs-l2-lowram").is_file(),
                         "needs local Limine and low-RAM artifacts")
    def test_serial_vm_boot_menu(self):
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp)
            (build / "out").mkdir()
            (build / "logs").mkdir()
            for name in ("vmlinuz-l2-lowram", "initramfs-l2-lowram"):
                (build / "out" / name).symlink_to(ROOT / "build-lowram/out" / name)
            subprocess.run([ROOT / "tools/l2-lowram", "iso"], check=True,
                           env=dict(os.environ, L2_BUILD_DIR=temp, L2_MAIN_BUILD_DIR=str(ROOT / "build")))
            config = (build / "iso.lowram/boot/limine/limine.conf").read_text()
            self.assertIn("timeout: 1\n", config)
            self.assertIn("l2.rescue=1", config)
            self.assertEqual(config, (build / "iso.lowram/EFI/BOOT/limine.conf").read_text())
