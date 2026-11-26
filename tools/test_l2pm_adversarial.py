"""Phase 04 adversarial fault-injection & security suite for l2pm (M20).

Probes tar extraction boundaries, privilege bits, archive truncation/padding,
GC fail-closed behavior under corrupt state, concurrent install/verify races,
and package name/dependency validation.
"""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

REPO = Path(__file__).resolve().parent.parent
L2PM = REPO / "l2pm" / "l2pm"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build_tar_bytes(entries: list[tuple]) -> bytes:
    """Build a ustar archive from tuples of (name, kind, payload_or_target, mode)."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for name, kind, data, mode in entries:
            ti = tarfile.TarInfo(name=name)
            ti.mode = mode
            ti.uid = 0
            ti.gid = 0
            ti.mtime = 1767225600
            if kind == "file":
                ti.type = tarfile.REGTYPE
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
            elif kind == "dir":
                ti.type = tarfile.DIRTYPE
                ti.size = 0
                tf.addfile(ti)
            elif kind == "symlink":
                ti.type = tarfile.SYMTYPE
                ti.linkname = data
                tf.addfile(ti)
            elif kind == "hardlink":
                ti.type = tarfile.LNKTYPE
                ti.linkname = data
                tf.addfile(ti)
            else:
                raise ValueError(kind)
    return buf.getvalue()


def make_l2p(path: Path, name: str, version: str, files: list[tuple]) -> Path:
    """Build a valid .l2p package archive with .l2pm/info + files."""
    info_bytes = f"name={name}\nversion={version}\nepoch=1767225600\ndeps=\n".encode()
    tar_entries = [(".l2pm/info", "file", info_bytes, 0o644)]
    for rel, content, mode in files:
        tar_entries.append((rel, "file", content, mode))
    path.write_bytes(build_tar_bytes(tar_entries))
    return path


class TestL2pmAdversarial(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not L2PM.exists():
            subprocess.run(["make", "-C", str(REPO / "l2pm")], check=True, capture_output=True)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="l2pm-adv-")
        self.addCleanup(self.tmp.cleanup)
        self.scratch = Path(self.tmp.name)
        self.root = self.scratch / "root"
        self.root.mkdir()
        self.env = dict(os.environ, L2PM_ROOT=str(self.root))

    def run_pm(self, *args, timeout=30):
        return subprocess.run(
            [str(L2PM), *args],
            env=self.env,
            cwd=str(self.scratch),
            capture_output=True,
            text=True,
            timeout=timeout,
        )

    def test_reject_symlink_escape_target(self):
        pkg = self.scratch / "sym-escape.l2p"
        info = b"name=symtest\nversion=1.0\nepoch=1767225600\ndeps=\n"
        data = build_tar_bytes([
            (".l2pm/info", "file", info, 0o644),
            ("bin/escape", "symlink", "../../../../../../etc/passwd", 0o777),
        ])
        pkg.write_bytes(data)
        r = self.run_pm("install", str(pkg))
        self.assertNotEqual(r.returncode, 0, "l2pm accepted escaping symlink target")

    def test_reject_hardlink_escape_target(self):
        pkg = self.scratch / "hardlink-escape.l2p"
        info = b"name=hltest\nversion=1.0\nepoch=1767225600\ndeps=\n"
        data = build_tar_bytes([
            (".l2pm/info", "file", info, 0o644),
            ("bin/hl", "hardlink", "../../etc/passwd", 0o644),
        ])
        pkg.write_bytes(data)
        r = self.run_pm("install", str(pkg))
        self.assertNotEqual(r.returncode, 0, "l2pm accepted escaping hardlink")

    def test_reject_setuid_setgid_bits(self):
        pkg = make_l2p(
            self.scratch / "suid.l2p",
            "suidpkg",
            "1.0",
            [("bin/suidtool", b"#!/bin/sh\necho hi\n", 0o4755)],
        )
        r = self.run_pm("install", str(pkg))
        if r.returncode == 0:
            # If installed, verify no setuid/setgid bit survived in store
            for p in (self.root / "l2" / "store").rglob("suidtool"):
                mode = p.stat().st_mode
                self.assertEqual(mode & 0o6000, 0, f"setuid/setgid bit preserved: {oct(mode)}")

    def test_reject_truncated_tar_archive(self):
        pkg = make_l2p(
            self.scratch / "trunc.l2p",
            "truncpkg",
            "1.0",
            [("bin/hello", b"A" * 4096, 0o755)],
        )
        raw = pkg.read_bytes()
        pkg.write_bytes(raw[: len(raw) // 2])
        r = self.run_pm("install", str(pkg))
        self.assertNotEqual(r.returncode, 0, "l2pm accepted truncated tar archive")

    def test_reject_bad_package_names(self):
        for bad_name in ("-leadingdash", "foo.manifest", "../escape", "a/b"):
            with self.subTest(name=bad_name):
                pkg = make_l2p(
                    self.scratch / f"bad-{abs(hash(bad_name))}.l2p",
                    bad_name,
                    "1.0",
                    [("bin/x", b"x", 0o755)],
                )
                r = self.run_pm("install", str(pkg))
                self.assertNotEqual(r.returncode, 0, f"accepted bad package name {bad_name!r}")

    def test_gc_fail_closed_on_corrupt_db(self):
        pkg = make_l2p(
            self.scratch / "good.l2p",
            "livepkg",
            "1.0",
            [("bin/livebin", b"live-content\n", 0o755)],
        )
        r_inst = self.run_pm("install", str(pkg))
        self.assertEqual(r_inst.returncode, 0, r_inst.stdout + r_inst.stderr)
        store_dir = self.root / "l2" / "store"
        before_store = sorted(p.name for p in store_dir.iterdir())
        self.assertTrue(before_store)

        # Corrupt installed DB entry
        db_dir = self.root / "l2" / "var" / "db" / "installed"
        (db_dir / "corrupt-entry").write_bytes(b"\x00\xffcorrupt\n")
        self.run_pm("gc")
        after_store = sorted(p.name for p in store_dir.iterdir())
        for entry in before_store:
            self.assertIn(entry, after_store, "GC deleted a live store path during corrupt DB state")

    def test_concurrent_install_stress(self):
        pkgs = [
            make_l2p(
                self.scratch / f"conc-{i}.l2p",
                f"concpkg{i}",
                "1.0",
                [(f"bin/tool{i}", f"payload-{i}\n".encode(), 0o755)],
            )
            for i in range(6)
        ]

        def install_one(p):
            return self.run_pm("install", str(p))

        with ThreadPoolExecutor(max_workers=4) as ex:
            results = list(ex.map(install_one, pkgs))
        failures = [r for r in results if r.returncode != 0]
        # Verify store consistency after concurrent installs
        r_ver = self.run_pm("verify")
        self.assertEqual(r_ver.returncode, 0, f"verify failed after concurrent installs: {r_ver.stdout}{r_ver.stderr}")
        self.assertEqual(failures, [], f"{len(failures)} concurrent install(s) failed")


if __name__ == "__main__":
    unittest.main()
