"""Regression tests for l2install safety checks and --dry-run fidelity.

Everything runs against scratch paths; no real disk is ever touched.
The dry-run test needs the prebuilt payloads in build/out (it skips
otherwise).  Offline and fast; wired into `make check`.
"""
import glob
import gzip
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parent.parent
# L2TEST_INSTALLER points the suite at a snapshot (e.g. git show HEAD:l2install)
# for fail-before/pass-after demonstrations; defaults to the working tree.
INSTALLER = Path(os.environ.get("L2TEST_INSTALLER", str(REPO / "l2install")))
OUT = REPO / "build" / "out"


def newc_archive(entries):
    """Minimal cpio 'newc' archive from (name, data, mode) tuples.

    Pure Python so tests can craft absolute-path members without creating
    anything outside the sandbox.
    """
    def member(name, data, mode):
        nb = name.encode() + b"\0"
        fields = (1, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(nb), 0)
        blob = b"070701" + b"".join(b"%08X" % v for v in fields) + nb
        blob += b"\0" * (-len(blob) % 4)
        blob += data + b"\0" * (-len(data) % 4)
        return blob
    return b"".join(member(n, d, m) for n, d, m in entries) + \
        member("TRAILER!!!", b"", 0)


def find_medium_devices():
    """Existing block devices in l2install find_medium / l2net find_pack order."""
    devs = []
    for pat in ("/dev/sr[0-9]", "/dev/sd[a-z]", "/dev/sd[a-z][0-9]",
                "/dev/vd[a-z]", "/dev/vd[a-z][0-9]",
                "/dev/nvme[0-9]n[0-9]", "/dev/nvme[0-9]n[0-9]p[0-9]",
                "/dev/mmcblk[0-9]", "/dev/mmcblk[0-9]p[0-9]"):
        devs.extend(sorted(glob.glob(pat)))
    return [d for d in devs if Path(d).is_block_device()]


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

    def test_image_requires_regular_target_even_with_confirmation(self):
        regular = self.scratch / "existing.img"
        regular.write_bytes(b"unchanged")
        link = self.scratch / "image-link"
        link.symlink_to(regular)
        dangling = self.scratch / "dangling"
        dangling.symlink_to(self.scratch / "missing")
        fifo = self.scratch / "pipe"
        os.mkfifo(fifo)
        for target in (link, dangling, fifo, self.scratch):
            with self.subTest(target=target):
                self.run_installer("--yes", "--dry-run", "image", str(target), ok=False)
        self.assertEqual(regular.read_bytes(), b"unchanged")

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

    # ------------------------------------- W6 P1 helpers (offline shims)
    def shimbin(self):
        d = self.scratch / "shimbin"
        d.mkdir(exist_ok=True)
        return d

    def add_shim(self, name, body):
        p = self.shimbin() / name
        p.write_text("#!/bin/sh\n" + body)
        p.chmod(0o755)
        return p

    def record_shims(self):
        """mount/umount recorders.  mount 'mounts' by copying the prepared
        tree $L2TEST_MOUNTMAP/<devname> onto the mountpoint; umount removes
        it again.  No real mount ever happens (tests run unprivileged), and
        the shims refuse to touch anything outside the sandbox even if a
        script under test ignores the path overrides (e.g. a HEAD snapshot)."""
        self.add_shim("mount",
                      "printf 'mount %s\\n' \"$*\" >> \"$L2TEST_SHIMLOG\"\n"
                      "if [ \"$1\" = -t ]; then dev=$5; mp=$6; else dev=$1; mp=$2; fi\n"
                      "case \"$mp\" in \"$L2TEST_SANDBOX\"/*) ;; *) exit 1 ;; esac\n"
                      "map=\"$L2TEST_MOUNTMAP/${dev##*/}\"\n"
                      "[ -d \"$map\" ] || exit 1\n"
                      "mkdir -p \"$mp\" || exit 1\n"
                      "cp -a \"$map/.\" \"$mp/\" || exit 1\n")
        self.add_shim("umount",
                      "printf 'umount %s\\n' \"$*\" >> \"$L2TEST_SHIMLOG\"\n"
                      "case \"${1:-}\" in \"$L2TEST_SANDBOX\"/*) rm -rf \"$1\" ;; esac\n")
        return dict(self.env,
                    PATH=f"{self.shimbin()}:{self.env['PATH']}",
                    L2TEST_SHIMLOG=str(self.scratch / "shim.log"),
                    L2TEST_MOUNTMAP=str(self.scratch / "mountmap"),
                    L2TEST_SANDBOX=str(self.scratch))

    def shim_lines(self, env):
        p = Path(env["L2TEST_SHIMLOG"])
        return p.read_text().splitlines() if p.exists() else []

    def cat_pid1_shim(self, comm):
        """Mock /proc/1/comm without giving production code an env bypass."""
        real = shutil.which("cat")
        self.add_shim("cat",
                      f"if [ \"${{1:-}}\" = /proc/1/comm ]; then printf '%s\\n' '{comm}'; "
                      f"else exec '{real}' \"$@\"; fi\n")

    def build_medium(self, root, marker=True, pack=True, absolute_name=None):
        """Fake L2-LIVE medium: boot files, install pack, gzip/newc initramfs
        whose rootfs carries a marker file.  absolute_name adds one member
        with an ABSOLUTE path (pass a path INSIDE the sandbox: a vulnerable
        cpio extraction then escapes the rootfs but stays in the sandbox)."""
        root = Path(root)
        (root / "boot" / "limine").mkdir(parents=True, exist_ok=True)
        (root / "EFI" / "BOOT").mkdir(parents=True, exist_ok=True)
        (root / "boot" / "vmlinuz").write_bytes(b"FAKE-KERNEL")
        (root / "boot" / "limine" / "limine-bios.sys").write_bytes(b"BIOS")
        (root / "EFI" / "BOOT" / "BOOTX64.EFI").write_bytes(b"EFI")
        entries = [
            ("bin", b"", 0o40755),
            ("bin/live-marker", b"L2-LIVE-ROOTFS\n", 0o100644),
        ]
        if absolute_name:
            entries.append((absolute_name, b"ABS\n", 0o100644))
        arc = newc_archive(entries)
        (root / "boot" / "initramfs").write_bytes(gzip.compress(arc, mtime=0))
        if marker:
            (root / "l2").mkdir(parents=True, exist_ok=True)
            (root / "l2" / "L2-LIVE").write_text("")
        if pack:
            bindir = root / "l2" / "install" / "bin"
            bindir.mkdir(parents=True, exist_ok=True)
            (root / "l2" / "install" / "etc").mkdir(parents=True, exist_ok=True)
            for tool in ("sgdisk", "limine"):
                p = bindir / tool
                p.write_text("#!/bin/sh\nexit 0\n")
                p.chmod(0o755)
        return root

    def pack_tool_shims(self, medium):
        """Recording cpio/install inside the pack's bin dir: if the installer
        prepends the pack to PATH (live substitution), these run."""
        bindir = Path(medium) / "l2" / "install" / "bin"
        for tool in ("cpio", "install"):
            real = shutil.which(tool)
            p = bindir / tool
            p.write_text(
                f"printf '{tool} %s\\n' \"$*\" >> \"$L2TEST_SHIMLOG\"\n"
                f"exec '{real}' \"$@\"\n")
            p.chmod(0o755)

    def run_env(self, env, *args, ok=True):
        r = subprocess.run([str(INSTALLER), *args], env=env,
                           cwd=str(self.scratch), capture_output=True,
                           text=True, timeout=120)
        if ok:
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        else:
            self.assertNotEqual(r.returncode, 0,
                                f"expected failure: {r.stdout} {r.stderr}")
        return r

    # ------------------------------------------- W6 P1: F3 medium trust
    def test_f3_host_without_build_tree_refuses_medium_tools(self):
        """F3: a host without a build tree must die with guidance, never
        substitute install tools from an inserted medium."""
        if Path("/proc/1/comm").read_text().strip() == "rcl2":
            self.skipTest("running inside the L2 live system; host path n/a")
        src = self.scratch / "empty-src"
        src.mkdir()
        self.build_medium(self.scratch / "media")  # L2_MEDIA points here
        env = self.record_shims()
        env["L2_SRC"] = str(src)
        self.cat_pid1_shim("init")  # name alone cannot authorize a medium
        r = self.run_env(env, "-y", "-n", "disk", "/dev/l2-fake-disk", ok=False)
        self.assertIn("build all", r.stderr)
        self.assertIn("live", r.stderr)
        self.assertNotIn("using install tools", r.stderr)
        # F14: the host path must not even mount the medium (no leak).
        self.assertFalse([l for l in self.shim_lines(env)
                          if l.startswith("mount ")])

    def test_f3_live_pid1_substitutes_medium_tools_and_payloads(self):
        """F3: the genuine live flow (PID1=rcl2, mocked via a cat shim) keeps
        substituting medium tools and payloads exactly as before."""
        src = self.scratch / "empty-src"
        src.mkdir()
        media = self.build_medium(self.scratch / "media")
        self.pack_tool_shims(media)
        env = self.record_shims()
        env["L2_SRC"] = str(src)
        self.cat_pid1_shim("rcl2")
        dest = self.scratch / "target-root"
        r = self.run_env(env, "--no-wifi", "--no-ssl", "dir", str(dest))
        self.assertIn("using install tools and payloads from the boot medium",
                      r.stderr)
        # payloads came from the medium's initramfs (the extracted rootfs)
        self.assertEqual((dest / "bin" / "live-marker").read_text(),
                         "L2-LIVE-ROOTFS\n")
        lines = self.shim_lines(env)
        # F3 defense in depth: where cpio supports --no-absolute-filenames
        # (GNU), the extraction must request it.  The live guest's busybox
        # cpio lacks the flag but strips leading '/' by default, so the
        # support probe must keep that flow working (no flag there).
        probe = subprocess.run(["cpio", "--no-absolute-filenames", "--help"],
                               capture_output=True)
        if probe.returncode == 0:
            self.assertTrue(any(l.startswith("cpio -idm") and
                                "--no-absolute-filenames" in l for l in lines),
                            lines)
        # the pack's bin dir was prepended to PATH: its 'install' ran
        self.assertTrue(any(l.startswith("install ") and "vmlinuz" in l
                            for l in lines), lines)

    def test_f3_cpio_absolute_member_stays_inside_rootfs(self):
        """F3 defense in depth: an initramfs member with an ABSOLUTE path
        must extract inside the staged rootfs, never at its absolute path.
        The absolute target is inside the sandbox, so even a vulnerable
        extraction (HEAD + GNU cpio) cannot write outside it."""
        src = self.scratch / "empty-src"
        src.mkdir()
        escape = self.scratch / "escape-probe"
        media = self.build_medium(self.scratch / "media",
                                  absolute_name=str(escape))
        self.pack_tool_shims(media)
        env = self.record_shims()
        env["L2_SRC"] = str(src)
        self.cat_pid1_shim("rcl2")
        dest = self.scratch / "target-root"
        self.run_env(env, "--no-wifi", "--no-ssl", "dir", str(dest))
        rel = escape.relative_to("/")  # stripped path, relative to rootfs
        self.assertTrue((dest / rel).exists(),
                        "absolute member must land inside the rootfs")
        self.assertFalse(escape.exists(),
                         "absolute member escaped the rootfs")

    def test_f14_live_medium_we_mounted_stays_mounted(self):
        """F14: on the live system a medium WE mounted stays mounted after
        cleanup (packs run from it); the host leak is closed by the F3 gate."""
        devs = find_medium_devices()
        if not devs:
            self.skipTest("no block device to simulate the boot medium")
        src = self.scratch / "empty-src"
        src.mkdir()
        self.build_medium(self.scratch / "mountmap" / Path(devs[0]).name)
        env = self.record_shims()
        env["L2_SRC"] = str(src)
        self.cat_pid1_shim("rcl2")
        r = self.run_env(env, "-y", "-n", "disk", "/dev/l2-fake-disk")
        self.assertIn("using install tools", r.stderr)
        lines = self.shim_lines(env)
        self.assertTrue(any(l.startswith("mount ") and devs[0] in l
                            for l in lines), lines)
        media = self.env["L2_MEDIA"]
        self.assertFalse([l for l in lines
                          if l.startswith("umount ") and media in l],
                         "live medium must stay mounted (packs run from it)")

    # ------------------------------------------- W6 P1: F4 image target
    def test_f4_image_refuses_existing_nonempty_without_yes(self):
        img = self.scratch / "existing.img"
        img.write_bytes(b"precious data\n" * 64)
        r = self.run_installer("-n", "image", str(img), ok=False)
        self.assertIn("refusing to overwrite", r.stderr)
        self.assertEqual(img.read_bytes(), b"precious data\n" * 64)
        # --yes is the explicit override (dry run then proceeds)
        r = self.run_installer("-y", "-n", "image", str(img), "256M")
        self.assertIn("truncate -s 256M", r.stdout + r.stderr)
        # an existing EMPTY file destroys nothing and stays allowed
        empty = self.scratch / "empty.img"
        empty.write_bytes(b"")
        self.run_installer("-n", "image", str(empty))

    def host_payload_env(self):
        rf = self.scratch / "rootfs"
        (rf / "bin").mkdir(parents=True)
        out = self.scratch / "out"
        (out / "limine" / "bin").mkdir(parents=True)
        (out / "vmlinuz-l2").write_bytes(b"k")
        (out / "initramfs-l2").write_bytes(b"i")
        lim = out / "limine" / "bin" / "limine"
        lim.write_text("#!/bin/sh\nexit 0\n")
        lim.chmod(0o755)
        return dict(self.record_shims(), L2_ROOTFS_DIR=str(rf), L2_OUT_DIR=str(out))

    # ------------------------------------------- W6 P1: F5 cleanup scope
    def test_f5_cleanup_only_umounts_what_we_mounted(self):
        """With an $MNT env override, cleanup must not umount a mount the
        installer never created (dry run mounts nothing at all)."""
        env = self.host_payload_env()
        self.run_env(env, "-y", "-n", "disk", "/dev/l2-fake-disk")
        self.assertFalse([l for l in self.shim_lines(env)
                          if l.startswith("umount ")],
                         "dry run mounted nothing; cleanup must umount nothing")

    def run_interrupted_install(self, env, sig, *args):
        # Deliver the signal during preflight, after the cleanup trap is set.
        self.add_shim("uname",
                      f'kill -s {sig.name[3:]} "$L2TEST_INSTALLER_PID"\n'
                      "printf 'x86_64\\n'\n")
        r = subprocess.run(
            ["/bin/sh", "-c", 'export L2TEST_INSTALLER_PID=$$; exec "$@"',
             "sh", str(INSTALLER), *args], env=env, cwd=self.scratch,
            capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 128 + sig, r.stdout + r.stderr)
        self.assertNotIn("sgdisk --zap-all", r.stderr)

    def test_interrupted_disk_dry_run_stops_before_partitioning(self):
        env = self.host_payload_env()
        for sig in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=sig):
                self.run_interrupted_install(env, sig, "-y", "-n", "disk",
                                             str(self.scratch / "fake-disk"))
                self.assertFalse(self.shim_lines(env))

    def test_interrupted_image_detaches_loop_once(self):
        env = self.host_payload_env()
        self.add_shim("id", "printf '0\\n'\n")
        self.add_shim("truncate", "exit 0\n")
        self.add_shim("losetup",
                      'if [ "$1" = -d ]; then\n'
                      '    printf "detach %s\\n" "$2" >> "$L2TEST_SHIMLOG"\n'
                      'else\n'
                      '    printf "%s/loop\\n" "$L2TEST_SANDBOX"\n'
                      'fi\n')
        image = self.scratch / "image.img"
        self.run_interrupted_install(env, signal.SIGTERM, "image", str(image))
        self.assertEqual(self.shim_lines(env), [f"detach {self.scratch / 'loop'}"])
        self.assertFalse(image.exists())

    # ------------------------------------------- W6 P1: F7 picker protocol
    def test_f7_model_cannot_inject_picker_protocol(self):
        fakebin = self.scratch / "bin"
        fakebin.mkdir()
        lsblk = fakebin / "lsblk"
        lsblk.write_text(
            "#!/bin/sh\n"
            "printf '/dev/sda 100G sata 0 Evil|/dev/sdb   999G   usb   removable   PWNED\\n'\n"
        )
        lsblk.chmod(0o755)
        env = dict(self.env, PATH=f"{fakebin}:{self.env['PATH']}")
        r = subprocess.run([str(INSTALLER), "list"], env=env,
                           cwd=str(self.scratch), capture_output=True,
                           text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("Evil", r.stdout)
        self.assertNotIn("|", r.stdout)

    # ------------------------------------------- W6 P1: F15 host-only image
    def test_f15_image_mode_host_only_guard(self):
        """image mode needs util-linux truncate+losetup (absent from the
        live guest's busybox): the guard must say host-only, point at disk
        mode, and name the missing tool."""
        minbin = self.scratch / "minbin"
        minbin.mkdir()
        for tool in ("mktemp", "dirname", "rm", "id", "cat", "sed"):
            real = shutil.which(tool)
            if real:
                (minbin / tool).symlink_to(real)
        env = dict(self.env, PATH=str(minbin))
        img = str(self.scratch / "x.img")
        r = self.run_env(env, "image", img, ok=False)
        self.assertIn("host-only", r.stderr)
        self.assertIn("l2install disk", r.stderr)
        self.assertIn("truncate", r.stderr)
        # with truncate present but losetup absent the guard names losetup
        (minbin / "truncate").symlink_to(shutil.which("truncate"))
        r = self.run_env(env, "image", img, ok=False)
        self.assertIn("host-only", r.stderr)
        self.assertIn("losetup", r.stderr)


if __name__ == "__main__":
    unittest.main()