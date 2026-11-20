"""Regression tests for l2pm archive-extraction hardening.

Crafts raw tar archives (including malicious ones) and runs `l2pm install`
under a scratch L2PM_ROOT.  Every rejection test asserts both a non-zero
exit and that nothing was written or modified outside the package root.
Offline and fast; wired into `make check`.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parent.parent
EPOCH = 315532800  # l2pm DEFAULT_EPOCH


def put_octal(buf, off, width, value):
    buf[off:off + width] = f"{value:0{width - 1}o}".encode() + b"\x00"


def tar_header(name, size, typeflag, mode=0o644, mtime=EPOCH, linkname="",
               prefix="", magic=b"ustar\x0000", base256_size=False):
    h = bytearray(512)
    nb = name.encode()
    assert len(nb) <= 100
    h[0:len(nb)] = nb
    put_octal(h, 100, 8, mode)
    put_octal(h, 108, 8, 0)
    put_octal(h, 116, 8, 0)
    if base256_size:
        h[124] = 0x80
        h[125:136] = size.to_bytes(11, "big")
    else:
        put_octal(h, 124, 12, size)
    put_octal(h, 136, 12, mtime)
    h[148:156] = b"        "
    h[156] = ord(typeflag)
    lb = linkname.encode()
    assert len(lb) <= 100
    h[157:157 + len(lb)] = lb
    h[257:257 + len(magic)] = magic
    h[265:269] = b"root"
    h[297:301] = b"root"
    pb = prefix.encode()
    assert len(pb) <= 155
    h[345:345 + len(pb)] = pb
    csum = sum(h)
    h[148:155] = f"{csum:06o}\x00".encode()
    h[155] = 0x20
    return bytes(h)


def tar_entry(name, payload=b"", typeflag="0", **kw):
    if typeflag in ("1", "2", "5"):
        size = 0
    else:
        size = len(payload)
    kw.setdefault("mode", 0o755 if typeflag == "5" else 0o644)
    h = tar_header(name, size, typeflag, **kw)
    pad = (-len(payload)) % 512
    return h + payload + b"\x00" * pad


def tar_archive(entries, end_blocks=2):
    return b"".join(entries) + b"\x00" * (512 * end_blocks)


def info_entry(name="testpkg", version="1.0"):
    payload = f"name={name}\nversion={version}\nepoch={EPOCH}\ndeps=\n".encode()
    return tar_entry(".l2pm/info", payload)


def snapshot(tree):
    """{relpath: ('dir', mode) | ('file', sha256) | ('link', target)}"""
    out = {}
    for p in sorted(tree.rglob("*")):
        rel = str(p.relative_to(tree))
        if p.is_symlink():
            out[rel] = ("link", os.readlink(p))
        elif p.is_dir():
            out[rel] = ("dir", p.stat().st_mode & 0o7777)
        else:
            out[rel] = ("file", hashlib.sha256(p.read_bytes()).hexdigest())
    return out


class L2pmSecurity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bin = REPO / "l2pm" / "l2pm"
        if not cls.bin.exists():
            subprocess.run(["make", "-C", str(REPO / "l2pm")], check=True,
                           capture_output=True)
        if not cls.bin.exists():
            raise unittest.SkipTest("l2pm binary unavailable")

    def setUp(self):
        self.scratch = Path(tempfile.mkdtemp(prefix="l2pm-sec-"))
        self.addCleanup(shutil.rmtree, self.scratch, True)
        self.root = self.scratch / "root"
        self.outside = self.scratch / "outside"
        self.outside.mkdir()
        (self.outside / "secret").write_text("CANARY\n")
        (self.outside / "sub").mkdir()
        (self.outside / "sub" / "nested").write_text("CANARY2\n")
        self.env = dict(os.environ, L2PM_ROOT=str(self.root),
                        L2PM_RECIPES=str(REPO / "l2pm" / "recipes"),
                        SOURCE_DATE_EPOCH=str(EPOCH))

    def install(self, archive, expect_ok=False, extra_args=()):
        pkg = self.scratch / "pkg.l2p"
        pkg.write_bytes(archive)
        before = snapshot(self.outside)
        r = subprocess.run([str(self.bin), "install", str(pkg), *extra_args],
                           env=self.env, capture_output=True, text=True, timeout=60)
        after = snapshot(self.outside)
        self.assertEqual(before, after,
                         f"files outside the package root changed!\n{r.stderr}")
        # nothing may appear anywhere in scratch outside root/ either
        for p in self.scratch.iterdir():
            if p.name not in ("root", "outside", "pkg.l2p"):
                self.fail(f"unexpected file created outside root: {p}")
        if expect_ok:
            self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")
        else:
            self.assertNotEqual(r.returncode, 0, "malicious archive was accepted")
            store = self.root / "l2" / "store"
            if store.exists():
                self.assertEqual(list(store.iterdir()), [],
                                 "rejected archive left a store path behind")
        return r

    def store_dir(self, archive, name="testpkg", version="1.0"):
        h = hashlib.sha256(archive).hexdigest()
        return self.root / "l2" / "store" / f"{h}-{name}-{version}"

    # ------------------------------------------------------- valid packages
    def test_valid_archive_installs(self):
        arc = tar_archive([
            info_entry(),
            tar_entry("usr/", typeflag="5", mode=0o755),
            tar_entry("usr/lib/", typeflag="5", mode=0o755),
            tar_entry("usr/bin/", typeflag="5", mode=0o755),
            tar_entry("usr/lib/tool", b"#!/bin/sh\necho hi\n", mode=0o755),
            tar_entry("usr/bin/tool", typeflag="2", linkname="../lib/tool"),
            tar_entry("usr/lib/tool-copy", typeflag="1", linkname="usr/lib/tool"),
            tar_entry("etc/", typeflag="5", mode=0o755),
            tar_entry("etc/conf", b"key=value\n"),
        ])
        r = self.install(arc, expect_ok=True)
        self.assertIn("installed testpkg 1.0", r.stdout)
        sd = self.store_dir(arc)
        self.assertEqual((sd / "usr/lib/tool").read_text(), "#!/bin/sh\necho hi\n")
        self.assertEqual(os.readlink(sd / "usr/bin/tool"), "../lib/tool")
        self.assertEqual((sd / "usr/lib/tool-copy").read_text(), "#!/bin/sh\necho hi\n")
        self.assertEqual((sd / "usr/lib/tool").stat().st_nlink, 2)
        self.assertEqual((sd / "usr/lib/tool").stat().st_mode & 0o7777, 0o755)
        prof = self.root / "l2/var/profiles/default/usr/bin/tool"
        self.assertTrue(prof.is_symlink())
        v = subprocess.run([str(self.bin), "verify"], env=self.env,
                           capture_output=True, text=True)
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)

    def test_contained_dotdot_symlink_allowed(self):
        arc = tar_archive([
            info_entry(),
            tar_entry("a/", typeflag="5"), tar_entry("a/b/", typeflag="5"),
            tar_entry("a/b/x", typeflag="2", linkname="../../c/y"),
            tar_entry("c/", typeflag="5"), tar_entry("c/y", b"ok\n"),
        ])
        self.install(arc, expect_ok=True)

    def test_duplicate_regular_entry_last_wins(self):
        arc = tar_archive([info_entry(),
                           tar_entry("f", b"first\n"), tar_entry("f", b"second\n")])
        self.install(arc, expect_ok=True)
        self.assertEqual((self.store_dir(arc) / "f").read_text(), "second\n")

    def test_single_zero_block_end_accepted(self):
        arc = tar_archive([info_entry(), tar_entry("f", b"x\n")], end_blocks=1)
        self.install(arc, expect_ok=True)

    def test_base256_size_accepted(self):
        payload = b"base256\n"
        e = tar_header("f", len(payload), "0", base256_size=True) + payload
        e += b"\x00" * ((-len(payload)) % 512)
        arc = tar_archive([info_entry(), e])
        self.install(arc, expect_ok=True)
        self.assertEqual((self.store_dir(arc) / "f").read_bytes(), payload)

    def test_deep_nesting_and_files_before_dirs(self):
        arc = tar_archive([info_entry(),
                           tar_entry("a/b/c/d/e/file", b"deep\n"),
                           tar_entry("a/", typeflag="5")])
        self.install(arc, expect_ok=True)
        self.assertEqual((self.store_dir(arc) / "a/b/c/d/e/file").read_text(), "deep\n")

    def test_readonly_dir_mode_applied_after_children(self):
        arc = tar_archive([info_entry(),
                           tar_entry("ro/", typeflag="5", mode=0o555),
                           tar_entry("ro/f", b"inside\n")])
        self.install(arc, expect_ok=True)
        sd = self.store_dir(arc)
        self.assertEqual((sd / "ro").stat().st_mode & 0o7777, 0o555)
        self.assertEqual((sd / "ro/f").read_text(), "inside\n")

    # ------------------------------------------------------ symlink escapes
    def test_symlink_escape_relative(self):
        arc = tar_archive([
            info_entry(),
            tar_entry("evil", typeflag="2", linkname="../../outside"),
            tar_entry("evil/pwned", b"ESCAPED\n"),
        ])
        self.install(arc)
        self.assertFalse((self.outside / "pwned").exists())

    def test_symlink_escape_absolute(self):
        arc = tar_archive([
            info_entry(),
            tar_entry("evil", typeflag="2", linkname=str(self.outside)),
            tar_entry("evil/pwned", b"ESCAPED\n"),
        ])
        self.install(arc)

    def test_symlink_escape_deep_dotdot(self):
        arc = tar_archive([
            info_entry(),
            tar_entry("a/", typeflag="5"), tar_entry("a/b/", typeflag="5"),
            tar_entry("a/b/c", typeflag="2", linkname="../../../../outside"),
            tar_entry("a/b/c/pwned", b"ESCAPED\n"),
        ])
        self.install(arc)

    def test_symlink_target_dot_stays_inside(self):
        # "." resolves to the entry's own directory: contained, so allowed,
        # and it must never point outside the package root.
        arc = tar_archive([info_entry(),
                           tar_entry("s", typeflag="2", linkname=".")])
        self.install(arc, expect_ok=True)
        self.assertEqual(os.readlink(self.store_dir(arc) / "s"), ".")

    def test_write_through_existing_symlink_same_name(self):
        # A symlink entry followed by a regular file of the SAME name must
        # replace the symlink, never write through it (which would corrupt
        # the link target).  Absolute targets are rejected outright, so the
        # tunnel attempt here uses a contained link.
        arc = tar_archive([
            info_entry(),
            tar_entry("other", b"ORIGINAL\n"),
            tar_entry("f", typeflag="2", linkname="other"),
            tar_entry("f", b"replaced\n"),
        ])
        self.install(arc, expect_ok=True)
        sd = self.store_dir(arc)
        self.assertFalse((sd / "f").is_symlink())
        self.assertEqual((sd / "f").read_text(), "replaced\n")
        self.assertEqual((sd / "other").read_text(), "ORIGINAL\n")

    def test_dir_entry_through_symlink_rejected(self):
        arc = tar_archive([
            info_entry(),
            tar_entry("d", typeflag="2", linkname=str(self.outside)),
            tar_entry("d/f", b"ESCAPED\n"),
        ])
        self.install(arc)
        self.assertFalse((self.outside / "f").exists())

    def test_symlink_with_payload_rejected(self):
        # type '2' with a nonzero size would desynchronize the stream.
        h = bytearray(tar_header("s", 0, "2", linkname="f"))
        put_octal(h, 124, 12, 4)
        csum = sum(h[:148]) + sum(b"        ") + sum(h[156:])
        h[148:155] = f"{csum:06o}\x00".encode()
        arc = tar_archive([info_entry(), bytes(h) + b"junk",
                           tar_entry("f", b"x\n")])
        self.install(arc)

    # ----------------------------------------------------- hardlink escapes
    def test_hardlink_escape_relative(self):
        arc = tar_archive([info_entry(),
                           tar_entry("h", typeflag="1",
                                     linkname="../../outside/secret")])
        self.install(arc)

    def test_hardlink_escape_absolute(self):
        arc = tar_archive([info_entry(),
                           tar_entry("h", typeflag="1", linkname="/etc/passwd")])
        self.install(arc)

    def test_hardlink_missing_target_rejected(self):
        arc = tar_archive([info_entry(),
                           tar_entry("h", typeflag="1", linkname="nonexistent")])
        self.install(arc)

    def test_hardlink_to_symlink_rejected(self):
        arc = tar_archive([info_entry(),
                           tar_entry("s", typeflag="2", linkname="f"),
                           tar_entry("f", b"x\n"),
                           tar_entry("h", typeflag="1", linkname="s")])
        self.install(arc)

    # --------------------------------------------------------- path attacks
    def test_dotdot_path_rejected(self):
        arc = tar_archive([info_entry(), tar_entry("../escape", b"x\n")])
        self.install(arc)
        self.assertFalse((self.scratch / "escape").exists())

    def test_nested_dotdot_path_rejected(self):
        arc = tar_archive([info_entry(), tar_entry("foo/../../escape", b"x\n")])
        self.install(arc)

    def test_absolute_path_rejected(self):
        arc = tar_archive([info_entry(),
                           tar_entry(str(self.outside / "abs"), b"x\n")])
        self.install(arc)

    def test_dot_component_rejected(self):
        arc = tar_archive([info_entry(), tar_entry("foo/./bar", b"x\n")])
        self.install(arc)

    def test_bare_dot_rejected(self):
        arc = tar_archive([info_entry(), tar_entry(".", typeflag="5")])
        self.install(arc)

    def test_empty_name_rejected(self):
        arc = tar_archive([info_entry(), tar_entry("", b"x\n")])
        self.install(arc)

    # --------------------------------------------------- corruption / format
    def test_truncated_payload_rejected(self):
        h = tar_header("f", 100, "0")
        arc = tar_archive([info_entry(), h + b"only-ten!!"])
        self.install(arc)

    def test_truncated_header_rejected(self):
        # one zero block, then a partial second block: truncated archive
        arc = tar_archive([info_entry()], end_blocks=1) + b"\x01" * 100
        self.install(arc)

    def test_oversized_size_rejected(self):
        arc = tar_archive([info_entry(), tar_header("f", 1 << 41, "0")])
        self.install(arc)

    def test_bad_checksum_rejected(self):
        h = bytearray(tar_entry("f", b"x\n")[:512])
        h[148] = ord("7") if h[148] != ord("7") else ord("6")
        arc = tar_archive([info_entry(), bytes(h) + b"x\n" + b"\x00" * 510])
        self.install(arc)

    def test_bad_magic_rejected(self):
        arc = tar_archive([info_entry(),
                           tar_entry("f", b"x\n", magic=b"gnutar\x00")])
        self.install(arc)

    def test_non_octal_size_rejected(self):
        h = bytearray(tar_header("f", 1, "0"))
        h[124:136] = b"12x4567890\x00\x00"
        csum = sum(h[:148]) + sum(b"        ") + sum(h[156:])
        h[148:155] = f"{csum:06o}\x00".encode()
        arc = tar_archive([info_entry(), bytes(h)])
        self.install(arc)

    def test_garbage_after_zero_block_rejected(self):
        # garbage where the second zero block must be
        arc = tar_archive([info_entry()], end_blocks=1) + b"JUNK" * 128
        self.install(arc)

    def test_pax_extended_header_rejected_cleanly(self):
        pax = b"20 path=verylongname\n"
        arc = tar_archive([
            info_entry(),
            tar_entry("PaxHeaders/f", pax, typeflag="x"),
            tar_entry("f", b"x\n"),
        ])
        r = self.install(arc)
        self.assertIn("extended tar metadata", r.stderr)

    def test_pax_global_header_rejected_cleanly(self):
        arc = tar_archive([info_entry(),
                           tar_entry("global", b"10 mtime=0\n", typeflag="g")])
        r = self.install(arc)
        self.assertIn("extended tar metadata", r.stderr)

    def test_gnu_longname_rejected_cleanly(self):
        arc = tar_archive([
            info_entry(),
            tar_entry("././@LongLink", b"a" * 150 + b"\x00", typeflag="L"),
            tar_entry("f", b"x\n"),
        ])
        r = self.install(arc)
        self.assertIn("extended tar metadata", r.stderr)

    def test_gnu_longlink_rejected_cleanly(self):
        arc = tar_archive([info_entry(),
                           tar_entry("././@LongLink", b"b" * 120 + b"\x00",
                                     typeflag="K")])
        r = self.install(arc)
        self.assertIn("extended tar metadata", r.stderr)

    def test_unsupported_type_rejected(self):
        arc = tar_archive([info_entry(), tar_entry("fifo", typeflag="6")])
        self.install(arc)

    # ----------------------------------------------------------- metadata
    def test_oversized_info_rejected_without_oom(self):
        arc = tar_archive([tar_entry(".l2pm/info", b"x" * (2 * 1024 * 1024)),
                           tar_entry("f", b"x\n")])
        self.install(arc)

    def test_missing_info_rejected(self):
        arc = tar_archive([tar_entry("f", b"x\n")])
        self.install(arc)

    def test_store_path_symlink_planted_rejected(self):
        arc = tar_archive([info_entry(), tar_entry("f", b"x\n")])
        h = hashlib.sha256(arc).hexdigest()
        store = self.root / "l2" / "store"
        store.mkdir(parents=True)
        os.symlink(str(self.outside), store / f"{h}-testpkg-1.0")
        pkg = self.scratch / "pkg.l2p"
        pkg.write_bytes(arc)
        r = subprocess.run([str(self.bin), "install", str(pkg)], env=self.env,
                           capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual((self.outside / "secret").read_text(), "CANARY\n")

    # ------------------------------------------------------- full lifecycle
    def test_install_verify_remove_gc(self):
        arc = tar_archive([info_entry(), tar_entry("bin/", typeflag="5"),
                           tar_entry("bin/hello", b"hi\n", mode=0o755)])
        self.install(arc, expect_ok=True)
        sd = self.store_dir(arc)
        self.assertTrue(sd.is_dir())
        # --prune-generations opts into legacy keep-active-only gc; the
        # default now keeps rollback history (see test_l2pm_transactions).
        for cmd, want in ((["verify"], 0), (["remove", "testpkg"], 0),
                          (["verify"], 0), (["gc", "--prune-generations"], 0)):
            r = subprocess.run([str(self.bin), *cmd], env=self.env,
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, want, r.stdout + r.stderr)
        self.assertFalse(sd.exists())

    def test_tar_cli_reproducible_with_symlinks(self):
        tree = self.scratch / "tree"
        (tree / "sub").mkdir(parents=True)
        (tree / "sub" / "file").write_text("data\n")
        os.symlink("../sub/file", tree / "link")
        a, b = self.scratch / "a.l2p", self.scratch / "b.l2p"
        for out in (a, b):
            r = subprocess.run([str(self.bin), "tar", str(tree), str(out),
                                "--epoch", str(EPOCH)], env=self.env,
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(hashlib.sha256(a.read_bytes()).hexdigest(),
                         hashlib.sha256(b.read_bytes()).hexdigest())
        # and the archive round-trips through install
        arc = a.read_bytes()
        pkg = self.scratch / "pkg.l2p"
        pkg.write_bytes(arc)
        # add info so it is installable
        r = subprocess.run([str(self.bin), "install", str(pkg)], env=self.env,
                           capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)  # no .l2pm/info -> refused

    def test_tar_cli_with_symlinks_and_files_roundtrips(self):
        tree = self.scratch / "symtree"
        (tree / ".l2pm").mkdir(parents=True)
        (tree / ".l2pm" / "info").write_text(
            f"name=sympkg\nversion=1.0\nepoch={EPOCH}\ndeps=\n"
        )
        (tree / "a_first.txt").write_text("first\n")
        os.symlink("z_last.txt", tree / "m_link")
        (tree / "z_last.txt").write_text("after symlink!\n")
        pkg = self.scratch / "sympkg.l2p"
        r = subprocess.run([str(self.bin), "tar", str(tree), str(pkg),
                            "--epoch", str(EPOCH)], env=self.env,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = subprocess.run([str(self.bin), "install", str(pkg)], env=self.env,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        sd = self.store_dir(pkg.read_bytes(), "sympkg", "1.0")
        self.assertEqual((sd / "a_first.txt").read_text(), "first\n")
        self.assertEqual(os.readlink(sd / "m_link"), "z_last.txt")
        self.assertEqual((sd / "z_last.txt").read_text(), "after symlink!\n")
        v = subprocess.run([str(self.bin), "verify"], env=self.env,
                           capture_output=True, text=True)
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)

    def test_dot_and_dotdot_names_rejected(self):
        for bad in (".", "..", ".hidden"):
            arc = tar_archive([info_entry(name=bad), tar_entry("f", b"x\n")])
            self.install(arc)
        arc = tar_archive([info_entry(), tar_entry("f", b"x\n")])
        for bad in ("..", ".hidden"):
            self.install(arc, extra_args=["--profile", bad])
        for cmd in (["info", ".."], ["info", ".hidden"],
                    ["remove", ".."], ["remove", ".hidden"]):
            r = subprocess.run([str(self.bin), *cmd], env=self.env,
                               capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0, f"{cmd} unexpectedly succeeded")

    def test_tar_rejects_fifo(self):
        tree = self.scratch / "fifotree"
        tree.mkdir()
        os.mkfifo(tree / "pipe")
        out = self.scratch / "fifo.l2p"
        r = subprocess.run([str(self.bin), "tar", str(tree), str(out)],
                           env=self.env, capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unsupported file type", r.stderr)
        self.assertFalse(out.exists())


class L2pmRecipeRepro(unittest.TestCase):
    """The shipped recipes must still build reproducibly after hardening."""

    @classmethod
    def setUpClass(cls):
        cls.bin = REPO / "l2pm" / "l2pm"
        if not cls.bin.exists():
            raise unittest.SkipTest("l2pm binary unavailable")

    def test_l2fetch_recipe_reproducible(self):
        with tempfile.TemporaryDirectory(prefix="l2pm-repro-") as tmp:
            env = dict(os.environ, L2PM_ROOT=tmp,
                       L2PM_RECIPES=str(REPO / "l2pm" / "recipes"),
                       SOURCE_DATE_EPOCH=str(EPOCH))
            recipe = REPO / "l2pm" / "recipes" / "l2fetch.recipe"
            r = subprocess.run([str(self.bin), "build", str(recipe), "--check"],
                               env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("reproducible: yes", r.stdout)


if __name__ == "__main__":
    unittest.main()