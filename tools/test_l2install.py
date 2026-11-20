"""Regression tests for l2install safety checks and --dry-run fidelity.

Everything runs against scratch paths; no real disk is ever touched.
The dry-run test needs the prebuilt payloads in build/out (it skips
otherwise).  Offline and fast; wired into `make check`.
"""
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parent.parent
INSTALLER = REPO / "l2install"
OUT = REPO / "build" / "out"


class L2install(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="l2install-test-")
        self.addCleanup(self.tmp.cleanup)
        self.scratch = Path(self.tmp.name)
        self.env = dict(os.environ,
                        TMPDIR=str(self.scratch),
                        MNT=str(self.scratch / "mnt"),
                        L2_MEDIA=str(self.scratch / "media"),
                        NO_COLOR="1", L2_PLAIN="1")

    def run_installer(self, *args, ok=True):
        r = subprocess.run([str(INSTALLER), *args], env=self.env,
                           capture_output=True, text=True, timeout=120,
                           cwd=str(self.scratch))
        if ok:
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        else:
            self.assertNotEqual(r.returncode, 0,
                                f"expected failure: {r.stdout} {r.stderr}")
        return r

    # ------------------------------------------------------- option guards
    def test_bad_hostname_rejected(self):
        r = self.run_installer("--hostname", "bad;name", "list", ok=False)
        self.assertIn("bad hostname", r.stderr)

    def test_bad_efi_size_rejected(self):
        r = self.run_installer("--efi-size", "32", "list", ok=False)
        self.assertIn("at least 64", r.stderr)
        r = self.run_installer("--efi-size", "abc", "list", ok=False)
        self.assertIn("bad --efi-size", r.stderr)

    def test_bad_firmware_rejected(self):
        r = self.run_installer("--firmware", "bogus", "list", ok=False)
        self.assertIn("bad --firmware", r.stderr)

    def test_list_runs(self):
        r = self.run_installer("list")
        # ui_table renders the CSV header as columns
        self.assertIn("DISK", r.stdout)
        self.assertIn("KIND", r.stdout)

    def test_image_refuses_block_device(self):
        devs = sorted(Path("/dev").glob("sd*")) + sorted(Path("/dev").glob("loop*"))
        devs = [d for d in devs if d.is_block_device()]
        if not devs:
            self.skipTest("no block device available to test refusal")
        r = self.run_installer("-n", "image", str(devs[0]), ok=False)
        self.assertIn("not a block device", r.stderr)

    def test_partition_target_refused(self):
        # find a real partition (has a 'partition' attribute in sysfs)
        part = None
        for d in sorted(Path("/sys/class/block").iterdir()):
            if (d / "partition").exists() and Path("/dev", d.name).is_block_device():
                part = Path("/dev", d.name)
                break
        if part is None:
            self.skipTest("no partition device available to test refusal")
        r = self.run_installer("-y", "-n", "disk", str(part), ok=False)
        self.assertIn("not a whole disk", r.stderr)

    # --------------------------------------------------------- dry run
    @unittest.skipUnless((OUT / "vmlinuz-l2").exists() and
                         (OUT / "initramfs-l2").exists() and
                         (OUT / "limine" / "bin" / "limine").exists(),
                         "prebuilt payloads missing")
    def test_dry_run_changes_nothing(self):
        target = self.scratch / "fake-disk.img"
        r = self.run_installer("-y", "-n", "disk", str(target))
        both = r.stdout + r.stderr
        self.assertIn("sgdisk --zap-all", both)
        self.assertIn("mkfs.ext4", both)
        self.assertIn("bios-install", both)
        self.assertFalse(target.exists(), "dry run created the target file")
        self.assertFalse((self.scratch / "mnt").exists(),
                         "dry run created the mount point")
        # the log must be a private mktemp file (0600), not a predictable name
        logs = list(self.scratch.glob("l2install.*"))
        self.assertEqual(len(logs), 1, f"unexpected temp files: {logs}")
        self.assertEqual(stat.S_IMODE(logs[0].stat().st_mode), 0o600)

    @unittest.skipUnless((OUT / "vmlinuz-l2").exists(), "prebuilt payloads missing")
    def test_dry_run_image_changes_nothing(self):
        target = self.scratch / "out.img"
        r = self.run_installer("-n", "image", str(target), "256M")
        self.assertIn("truncate -s 256M", r.stdout + r.stderr)
        self.assertFalse(target.exists())

    @unittest.skipUnless((REPO / "build/rootfs/bin").exists() and
                         (OUT / "vmlinuz-l2").exists(),
                         "prebuilt rootfs/kernel missing")
    def test_dir_install_writes_hosts_and_preserves_version(self):
        dest = self.scratch / "target-root"
        self.run_installer("--hostname", "l2box", "--no-wifi", "--no-ssl", "dir", str(dest))
        hosts = (dest / "etc/hosts").read_text()
        self.assertIn("127.0.0.1 localhost l2box", hosts)
        self.assertIn("::1 localhost", hosts)
        self.assertEqual((dest / "etc/l2/version").read_text().strip(), "L2 0.4.0")

    def test_list_unescapes_model_without_stripping_x20_chars(self):
        fakebin = self.scratch / "bin"
        fakebin.mkdir()
        lsblk = fakebin / "lsblk"
        lsblk.write_text(
            "#!/bin/sh\n"
            "printf '/dev/sda 100G sata 0 Samsung\\134x20SSD\\134x20860\\134x20EVO\\134x202TB\\n'\n"
        )
        lsblk.chmod(0o755)
        env = dict(self.env, PATH=f"{fakebin}:{self.env['PATH']}")
        r = subprocess.run([str(INSTALLER), "list"], env=env,
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("Samsung SSD 860 EVO 2TB", r.stdout)


if __name__ == "__main__":
    unittest.main()