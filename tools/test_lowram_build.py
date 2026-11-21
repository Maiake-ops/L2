"""Regression checks for the VM image builder in private output directories."""
import gzip
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

    def test_initramfs_is_independent_of_input_member_order(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            source = base / "files"
            (source / "etc").mkdir(parents=True)
            (source / "etc/rcl2.conf").write_text("hostname L2\n")
            for name in ("alpha", "beta", "gamma"):
                (source / name).write_text(name)
            images = []
            for number, names in enumerate((["alpha", "beta", "gamma"], ["gamma", "beta", "alpha"])):
                build = base / str(number)
                payload = build / "out"
                payload.mkdir(parents=True)
                members = "\n".join([".", "etc", "etc/rcl2.conf", *names, ""])
                archive = subprocess.check_output(["cpio", "-o", "-H", "newc", "--quiet"],
                                                  input=members.encode(), cwd=source)
                (payload / "initramfs-l2").write_bytes(gzip.compress(archive, mtime=0))
                subprocess.run([ROOT / "tools/l2-lowram", "initramfs"], check=True,
                               env=dict(os.environ, L2_BUILD_DIR=str(build), L2_MAIN_BUILD_DIR=str(build)))
                images.append((payload / "initramfs-l2-lowram").read_bytes())
            self.assertEqual(images[0], images[1])

    def test_release_rejects_unpinned_or_oversize_media_before_boot(self):
        with tempfile.TemporaryDirectory() as temp:
            image = Path(temp) / "candidate.iso"
            for size, message in ((32, "digest mismatch"), (10 * 1024**2, "size outside")):
                with image.open("wb") as stream:
                    stream.truncate(size)
                result = subprocess.run(["python3", ROOT / "tools/verify-lowram.py", image, "0" * 64],
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 2)
                self.assertIn(message, result.stderr)
