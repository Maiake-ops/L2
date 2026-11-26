"""Regression checks for the VM image builder in private output directories."""
import ast
import gzip
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LowramBuild(unittest.TestCase):
    def test_kernel_patch_mismatch_preserves_earlier_files(self):
        patcher = ROOT / "tools/patch-kernel-ram.py"
        # Use the first group's expected source text to make a minimal fixture.
        first_patch = next(
            node for node in ast.walk(ast.parse(patcher.read_text(encoding="utf-8")))
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
            and node.func.id == "patch_file"
        )
        original = "\n".join(old for _, old, _ in ast.literal_eval(first_patch.args[1]))
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp)
            first = source / "mm/page_alloc.c"
            first.parent.mkdir()
            first.write_text(original)
            later = source / "drivers/nvme/host/pci.c"
            later.parent.mkdir(parents=True)
            later.write_text("source differs from expected patch input\n")

            for _ in range(2):
                result = subprocess.run(
                    ["python3", patcher, source], capture_output=True, text=True, timeout=10,
                )
                self.assertEqual(result.returncode, 1)
                self.assertIn("hunk 'nvme_max_hmb_small' expected 1 match, found 0", result.stderr)
                self.assertEqual(first.read_text(), original)

    def test_kernel_configuration_failure_stops_build(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "linux-fixture"
            (source / "scripts/kconfig").mkdir(parents=True)
            (source / "arch/x86/boot").mkdir(parents=True)
            (source / "arch/x86/boot/bzImage").write_bytes(bytes(768))
            (source / ".config").write_text("CONFIG_TEST=y\n")
            (source / "Makefile").write_text(
                'tinyconfig olddefconfig:\n\t@test "$@" != "$$FAIL_STAGE"\n'
                'bzImage:\n\t@:\n'
                'kernelrelease:\n\t@echo fixture\n'
            )
            for name, command in {
                "kconfig/merge_config.sh": "exit 0",
                "config": 'test "$2" != "$FAIL_STAGE"',
            }.items():
                script = source / "scripts" / name
                script.write_text(f"#!/bin/sh\n{command}\n")
                script.chmod(0o755)
            (root / "tools").mkdir()
            (root / "tools/patch-kernel-ram.py").touch()
            (root / "config").mkdir()
            (root / "config/kernel.lowram").write_text("CONFIG_TEST=y\n")
            (root / "build/cache").mkdir(parents=True)
            with tarfile.open(root / "build/cache/linux-fixture.tar.xz", "w:xz") as tar:
                tar.add(source, arcname=source.name)

            for stage in ("tinyconfig", "olddefconfig", "LOCALVERSION", "LOCALVERSION_AUTO", "MODULES"):
                with self.subTest(stage=stage):
                    build = root / stage
                    result = subprocess.run(
                        [ROOT / "tools/l2-lowram", "build"],
                        env=dict(os.environ, L2_ROOT=temp, L2_BUILD_DIR=str(build),
                                 L2_MAIN_BUILD_DIR=str(root / "build"), KERNEL_VERSION="fixture",
                                 FAIL_STAGE=stage),
                        capture_output=True, text=True, timeout=10,
                    )
                    self.assertIn("kernel configure failed", result.stderr)
                    self.assertEqual(result.returncode, 1)
                    self.assertFalse((build / "out/vmlinuz-l2-lowram").exists())

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
            self.assertIn("console=ttyS0,115200", config)
            self.assertNotIn("console=tty0", config)
            self.assertNotIn("mem=1G", config)
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

    def test_check_dropped_treats_absent_disabled_as_ok_and_reports_real_drops(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            cfg = base / ".config"
            frag = base / "kernel.lowram"
            logs = base / "logs"
            cfg.write_text("CONFIG_KEEP=y\n# CONFIG_EXPLICIT_N is not set\n", encoding="utf-8")
            frag.write_text(
                "CONFIG_KEEP=y\n"
                "CONFIG_EXPLICIT_N=n\n"
                "CONFIG_ABSENT_N=n\n"
                "CONFIG_DROPPED_Y=y\n",
                encoding="utf-8",
            )
            script = (
                f'FRAGMENT="{frag}"\n'
                f'LOGS="{logs}"\n'
                'warn() { printf "%s\\n" "$*" >&2; }\n'
                + "\n".join(
                    (ROOT / "tools/l2-lowram").read_text(encoding="utf-8").splitlines()[103:134]
                )
                + f'\ncheck_dropped "{cfg}"\n'
            )
            r = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=10)
            self.assertEqual(r.returncode, 0, r.stderr)
            dropped = (logs / "kernel-lowram-dropped.txt").read_text(encoding="utf-8")
            self.assertIn("CONFIG_DROPPED_Y=y (got absent)", dropped)
            self.assertNotIn("CONFIG_ABSENT_N", dropped)
            self.assertNotIn("CONFIG_EXPLICIT_N", dropped)

    def test_initramfs_atomic_failure_does_not_publish_partial_file(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "out").mkdir()
            (base / "out/initramfs-l2").write_bytes(gzip.compress(b"not-a-valid-cpio", mtime=0))
            fakebin = base / "bin"
            fakebin.mkdir()
            # Force gzip failure during repack so atomic publish must not leave output.
            out_img = base / "out/initramfs-l2-lowram"
            r = subprocess.run(
                [ROOT / "tools/l2-lowram", "initramfs"],
                env=dict(os.environ, L2_BUILD_DIR=str(base), L2_MAIN_BUILD_DIR=str(base)),
                capture_output=True, text=True, timeout=10,
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertFalse(out_img.exists())
            self.assertEqual(list((base / "out").glob("initramfs-l2-lowram.tmp.*")), [])
