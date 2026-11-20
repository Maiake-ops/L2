"""Regression tests for l2pm generations, rollback, dependency resolution, and store integrity."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parent.parent
EPOCH = 315532800


class L2pmFeatures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bin = REPO / "l2pm" / "l2pm"
        subprocess.run(["make", "-C", str(REPO / "l2pm")], check=True, capture_output=True)

    def setUp(self):
        self.scratch = Path(tempfile.mkdtemp(prefix="l2pm-feat-"))
        self.addCleanup(shutil.rmtree, self.scratch, True)
        self.root = self.scratch / "root"
        self.recipes = self.scratch / "recipes"
        self.recipes.mkdir()
        self.env = dict(
            os.environ,
            L2PM_ROOT=str(self.root),
            L2PM_RECIPES=str(self.recipes),
            SOURCE_DATE_EPOCH=str(EPOCH),
        )

    def l2pm(self, *args, ok=True):
        r = subprocess.run(
            [str(self.bin), *args],
            env=self.env,
            cwd=str(self.scratch),
            capture_output=True,
            text=True,
            timeout=60,
        )
        if ok:
            self.assertEqual(r.returncode, 0, f"l2pm {' '.join(args)} failed:\n{r.stdout}\n{r.stderr}")
        else:
            self.assertNotEqual(r.returncode, 0, f"expected failure: {r.stdout}\n{r.stderr}")
        return r

    def write_recipe(self, name, version="1.0", deps="", marker=None):
        body = marker or f"{name}-{version}"
        (self.recipes / f"{name}.recipe").write_text(
            f"name = {name}\n"
            f"version = {version}\n"
            f"epoch = {EPOCH}\n"
            f"deps = {deps}\n"
            f"install = mkdir -p \"$DESTDIR/usr/bin\" && printf '%s\\n' '#!/bin/sh' 'echo {body}' > \"$DESTDIR/usr/bin/{name}\" && chmod 0755 \"$DESTDIR/usr/bin/{name}\"\n"
        )

    def test_generations_and_rollback(self):
        self.write_recipe("app", version="1.0")
        self.l2pm("add", "app")
        prof = self.root / "l2/var/profiles/default"
        self.assertTrue(prof.is_symlink())
        self.assertEqual(os.readlink(prof), "default-1-link")

        self.write_recipe("app", version="2.0")
        self.l2pm("add", "app")
        self.assertEqual(os.readlink(prof), "default-2-link")

        gens = self.l2pm("generations").stdout
        self.assertIn("1\t1 package(s)", gens)
        self.assertIn("2\t1 package(s)\t(current)", gens)
        self.assertEqual(subprocess.check_output([str(prof / "usr/bin/app")], text=True).strip(), "app-2.0")

        # Keep generations across GC so rollback still succeeds.
        self.l2pm("gc", "--keep-generations")
        self.l2pm("rollback")
        self.assertEqual(os.readlink(prof), "default-1-link")
        self.assertEqual(subprocess.check_output([str(prof / "usr/bin/app")], text=True).strip(), "app-1.0")
        info = self.l2pm("info", "app").stdout
        self.assertIn("version=1.0", info)
        self.l2pm("verify")

        # Explicit forward rollback to generation 2 also works.
        self.l2pm("rollback", "2")
        self.assertEqual(os.readlink(prof), "default-2-link")
        self.assertEqual(subprocess.check_output([str(prof / "usr/bin/app")], text=True).strip(), "app-2.0")

    def test_deps_resolution_cycle_and_remove_guard(self):
        self.write_recipe("libbase", version="1.0")
        self.write_recipe("libmid", version="1.0", deps="libbase")
        self.write_recipe("topapp", version="1.0", deps="libmid")

        self.l2pm("add", "topapp")
        lst = self.l2pm("list").stdout
        for name in ("libbase", "libmid", "topapp"):
            self.assertIn(name, lst)

        # Reverse dependency guard blocks removing libbase while libmid depends on it.
        r = self.l2pm("remove", "libbase", ok=False)
        self.assertIn("required by: libmid", r.stderr)

        # --force overrides reverse-dep guard.
        self.l2pm("remove", "--force", "libbase")
        # Re-add libbase with --no-deps test for another package.
        self.write_recipe("lonely", version="1.0", deps="nonexistent_dep")
        self.l2pm("add", "lonely", "--no-deps")
        self.assertIn("lonely", self.l2pm("list").stdout)

        # Cycle detection.
        self.write_recipe("cyca", deps="cycb")
        self.write_recipe("cycb", deps="cyca")
        rc = self.l2pm("add", "cyca", ok=False)
        self.assertIn("cycle", rc.stderr)

    def test_verify_detects_tampering_deletion_and_extra_file(self):
        self.write_recipe("tamper", version="1.0")
        self.l2pm("add", "tamper")
        self.l2pm("verify")

        store = next((self.root / "l2/store").iterdir())
        target = store / "usr/bin/tamper"
        orig = target.read_bytes()

        # 1. Modified file
        target.write_bytes(b"corrupted\n")
        r = self.l2pm("verify", ok=False)
        self.assertIn("BROKEN tamper: store integrity check failed", r.stdout)

        # Restore, then test extra unexpected file
        target.write_bytes(orig)
        self.l2pm("verify")
        extra = store / "usr/bin/evil"
        extra.write_text("unexpected\n")
        r = self.l2pm("verify", ok=False)
        self.assertIn("BROKEN tamper: store integrity check failed", r.stdout)

        # Remove extra, then delete tracked file
        extra.unlink()
        target.unlink()
        r = self.l2pm("verify", ok=False)
        self.assertIn("BROKEN tamper: store integrity check failed", r.stdout)

    def test_search_includes_cached_recipe_repo(self):
        self.write_recipe("localpkg")
        repo = self.root / "l2/var/recipe-repo"
        repo.mkdir(parents=True)
        (repo / "remotepkg.recipe").write_text("name=remotepkg\nversion=1\nbuild=true\n")
        out = self.l2pm("search").stdout.splitlines()
        self.assertEqual(out, ["localpkg.recipe", "remotepkg.recipe"])


if __name__ == "__main__":
    unittest.main()
