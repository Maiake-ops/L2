"""Regression tests for l2net pack trust (N1) and modalias hardening (N3).

Offline and sandboxed: mount/umount/modprobe/sleep are PATH shims (no real
mount or module load ever happens), and the hardcoded system paths are
redirected via L2_MEDIUM / L2_MODDIR / L2_FW_PATH — minimal env overrides
whose defaults are byte-identical to the real paths when unset.  The script
runs under the shipped static dash (build/out/dash) because hosts typically
lack /bin/dash; that is also the exact interpreter the live system uses.
L2TEST_L2NET points the suite at a snapshot for fail-before demonstrations.
W6 P1 (ARMAGEDDON II); wired into `make check` by the lead.
"""
import glob
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parent.parent
L2NET = Path(os.environ.get("L2TEST_L2NET", str(REPO / "overlay" / "bin" / "l2net")))
DASH = REPO / "build" / "out" / "dash"


def loop_devices():
    """Existing block devices in find_pack's search order."""
    devs = []
    for pat in ("/dev/sr[0-9]", "/dev/sd[a-z]", "/dev/sd[a-z][0-9]",
                "/dev/vd[a-z]", "/dev/vd[a-z][0-9]",
                "/dev/nvme[0-9]n[0-9]", "/dev/nvme[0-9]n[0-9]p[0-9]",
                "/dev/mmcblk[0-9]", "/dev/mmcblk[0-9]p[0-9]"):
        devs.extend(sorted(glob.glob(pat)))
    return [d for d in devs if Path(d).is_block_device()]


def any_boot_device():
    """find_pack's 'is there any boot device at all' probe (whole disks)."""
    for pat in ("/dev/sr[0-9]", "/dev/sd[a-z]", "/dev/vd[a-z]",
                "/dev/nvme[0-9]n[0-9]", "/dev/mmcblk[0-9]"):
        if any(Path(d).is_block_device() for d in glob.glob(pat)):
            return True
    return False


@unittest.skipUnless(DASH.exists(), "shipped dash (build/out/dash) missing")
class L2net(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="l2net-test-")
        self.addCleanup(self.tmp.cleanup)
        self.scratch = Path(self.tmp.name)
        self.krel = Path("/proc/sys/kernel/osrelease").read_text().strip()
        self.media = self.scratch / "media"
        self.mods = self.scratch / "modules"
        self.fwpath = self.scratch / "fwpath"
        self.fwpath.touch()  # use_pack only writes when the path is -w
        self.log = self.scratch / "shim.log"
        self.mountmap = self.scratch / "mountmap"
        self.mountmap.mkdir()
        self.shimbin = self.scratch / "shimbin"
        self.shimbin.mkdir()
        # Shims record every call but only ever mutate paths INSIDE the
        # sandbox — a script under test that ignores the overrides (e.g. a
        # HEAD snapshot without them) can never touch real system paths.
        self._shim("mount",
                   "printf 'mount %s\\n' \"$*\" >> \"$L2TEST_SHIMLOG\"\n"
                   "if [ \"$1\" = -t ]; then dev=$5; mp=$6; else dev=$1; mp=$2; fi\n"
                   "case \"$mp\" in \"$L2TEST_SANDBOX\"/*) ;; *) exit 1 ;; esac\n"
                   "map=\"$L2TEST_MOUNTMAP/${dev##*/}\"\n"
                   "[ -d \"$map\" ] || exit 1\n"
                   "mkdir -p \"$mp\" || exit 1\n"
                   "cp -a \"$map/.\" \"$mp/\" || exit 1\n")
        self._shim("umount",
                   "printf 'umount %s\\n' \"$*\" >> \"$L2TEST_SHIMLOG\"\n"
                   "case \"${1:-}\" in \"$L2TEST_SANDBOX\"/*) rm -rf \"$1\" ;; esac\n")
        self._shim("modprobe",
                   "printf 'modprobe %s\\n' \"$*\" >> \"$L2TEST_SHIMLOG\"\n")
        # no-op sleep: find_pack's bounded 10x retry loop stays fast offline
        self._shim("sleep",
                   "printf 'sleep %s\\n' \"$*\" >> \"$L2TEST_SHIMLOG\"\n")
        self.env = dict(os.environ,
                        TMPDIR=str(self.scratch),
                        PATH=f"{self.shimbin}:{os.environ['PATH']}",
                        L2_MEDIUM=str(self.media),
                        L2_MODDIR=str(self.mods),
                        L2_FW_PATH=str(self.fwpath),
                        L2TEST_SHIMLOG=str(self.log),
                        L2TEST_MOUNTMAP=str(self.mountmap),
                        L2TEST_SANDBOX=str(self.scratch))

    def _shim(self, name, body):
        p = self.shimbin / name
        p.write_text("#!/bin/sh\n" + body)
        p.chmod(0o755)

    def run_net(self, *args, ok=True, env=None):
        r = subprocess.run([str(DASH), str(L2NET), *args],
                           env=env or self.env, cwd=str(self.scratch),
                           capture_output=True, text=True, timeout=60)
        if ok:
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        else:
            self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
        return r

    def lines(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def make_pack(self, root, kind, marker=False, tag="GENUINE"):
        """root/l2/<kind> pack tree; marker adds root/l2/L2-LIVE.  The tag
        file inside lib/modules/$KREL tells genuine and attacker apart."""
        root = Path(root)
        pack = root / "l2" / kind
        moddir = pack / "lib" / "modules" / self.krel
        moddir.mkdir(parents=True, exist_ok=True)
        (moddir / tag).write_text(tag + "\n")
        (pack / "lib" / "firmware").mkdir(parents=True, exist_ok=True)
        (pack / "lib" / "firmware" / "dummy.fw").write_bytes(b"fw")
        wpa = pack / "bin" / "wpa_supplicant"
        wpa.parent.mkdir(parents=True, exist_ok=True)
        wpa.write_text("#!/bin/sh\nexit 0\n")
        wpa.chmod(0o755)
        if marker:
            (root / "l2" / "L2-LIVE").write_text("")
        return pack

    def installed_system(self):
        d = self.mods / self.krel
        d.mkdir(parents=True, exist_ok=True)
        (d / "modules.dep").write_text("")
        return d

    def pack_link(self):
        return self.mods / self.krel

    # ------------------------------------------------ N1: pack trust chain
    def test_n1_installed_short_circuit_wins(self):
        """Installed system: /lib/modules/$KREL is the first line of defense;
        no medium is consulted even when an attacker pack is pre-mounted."""
        self.installed_system()
        self.make_pack(self.media, "wifi", marker=False, tag="ATTACKER")
        self.run_net("modules")
        self.assertFalse(self.pack_link().is_symlink())
        self.assertTrue((self.pack_link() / "modules.dep").exists())
        self.assertEqual(self.fwpath.read_text(), "")
        self.assertEqual(self.lines(), [])

    def test_n1_premounted_medium_without_marker_refused(self):
        """N1: a pre-mounted medium carrying l2/wifi + l2/hw but NO L2-LIVE
        marker must never supply modules/firmware/binaries."""
        self.make_pack(self.media, "wifi", marker=False, tag="ATTACKER")
        self.make_pack(self.media, "hw", marker=False, tag="ATTACKER")
        self.run_net("modules")
        self.assertFalse(self.pack_link().exists())
        self.assertEqual(self.fwpath.read_text(), "")

    def test_n1_premounted_live_marker_accepted(self):
        """Preserved: the genuine boot medium (marker + pack) still works."""
        pack = self.make_pack(self.media, "wifi", marker=True)
        self.run_net("modules")
        link = self.pack_link()
        self.assertTrue(link.is_symlink())
        self.assertEqual(os.readlink(link),
                         str(pack / "lib" / "modules" / self.krel))
        self.assertEqual(self.fwpath.read_text().strip(),
                         str(pack / "lib" / "firmware"))

    def test_n1_loop_attacker_medium_first_refused(self):
        """N1 peer exploit: an attacker CD wins the device loop order over
        the USB boot medium.  Without the marker it must be umounted and
        skipped; only the marked boot medium may activate a pack."""
        devs = loop_devices()
        if not devs or not any_boot_device():
            self.skipTest("no block device to simulate inserted media")
        self.make_pack(self.mountmap / Path(devs[0]).name, "hw",
                       marker=False, tag="ATTACKER")
        genuine = None
        if len(devs) > 1:
            genuine = self.make_pack(self.mountmap / Path(devs[1]).name, "hw",
                                     marker=True, tag="GENUINE")
        self.run_net("modules")
        link = self.pack_link()
        if genuine is None:
            self.assertFalse(link.exists(),
                             "unmarked medium must never supply a pack")
        else:
            self.assertTrue(link.is_symlink())
            self.assertTrue((link / "GENUINE").exists())
            self.assertFalse((link / "ATTACKER").exists())
        # the attacker medium was released, not left mounted
        self.assertTrue(any(l.startswith("umount ") for l in self.lines()))

    def test_n1_live_marker_without_pack_stops_search(self):
        """Preserved: L2-LIVE without a pack exits after ONE mount attempt
        (the 10s boot trim), not after 10 retries."""
        devs = loop_devices()
        if not devs or not any_boot_device():
            self.skipTest("no block device to simulate the boot medium")
        m = self.mountmap / Path(devs[0]).name
        (m / "l2").mkdir(parents=True)
        (m / "l2" / "L2-LIVE").write_text("")
        self.run_net("modules")
        mounts = [l for l in self.lines() if l.startswith("mount ")]
        sleeps = [l for l in self.lines() if l.startswith("sleep ")]
        self.assertEqual(len(mounts), 1,
                         "must stop after the live medium, not retry 10x")
        self.assertEqual(sleeps, [])
        self.assertFalse(self.pack_link().exists())

    def test_n1_pack_dev_override_preserved(self):
        """Preserved: L2_PACK_DEV is still honored first in the loop."""
        devs = loop_devices()
        if not devs or not any_boot_device():
            self.skipTest("no block device for L2_PACK_DEV")
        self.make_pack(self.mountmap / Path(devs[0]).name, "hw", marker=True)
        env = dict(self.env, L2_PACK_DEV=devs[0])
        self.run_net("modules", env=env)
        self.assertTrue((self.pack_link() / "GENUINE").exists())

    # ------------------------------------------------ N3: modalias globs
    def probe_function(self):
        """probe_modaliases() extracted VERBATIM from the script under test.

        Function-level unit test: the host's real modaliases may contain no
        glob characters at all (this kernel emits fully-specified ones), and
        redirecting /sys/bus would be scope creep — so drive the exact
        function body with crafted modalias files instead."""
        src = L2NET.read_text().splitlines()
        start = next(i for i, l in enumerate(src)
                     if l.startswith("probe_modaliases()"))
        end = next(i for i in range(start + 1, len(src)) if src[i] == "}")
        return "\n".join(src[start:end + 1])

    def test_n3_modaliases_not_pathname_expanded(self):
        """N3: modalias words contain shell globs (*); the unquoted split
        must word-split but never pathname-expand against the cwd.  Plant a
        file matching the alias's glob prefix: expansion would feed the
        FILENAME to modprobe instead of the alias."""
        alias = "pci:v00001234d00005678sv*sd*bc*sc*i*"
        plain = "pci:v00009999d00008888sv00000000sd00000000bc06sc00i00"
        f1 = self.scratch / "alias-glob"
        f1.write_text(alias + "\n")
        f2 = self.scratch / "alias-plain"
        f2.write_text(plain + "\n")
        planted = alias.split("*")[0] + "GLOBMATCH"
        (self.scratch / planted).write_text("x")
        harness = self.probe_function() + '\nprobe_modaliases "$@"\n'
        r = subprocess.run([str(DASH), "-c", harness, "sh", str(f1), str(f2)],
                           env=self.env, cwd=str(self.scratch),
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        args = []
        for l in self.lines():
            if l.startswith("modprobe "):
                args.extend(l.split()[1:])
        self.assertIn(alias, args)
        self.assertIn(plain, args)  # word splitting must still happen
        self.assertNotIn(planted, args)


if __name__ == "__main__":
    unittest.main()
