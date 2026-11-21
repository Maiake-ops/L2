"""Regression tests for l2pm transaction atomicity, torn-state recovery and
dependency-solver edge cases (ARMAGEDDON II W2, missions 41-50 / 181-200).

Failure contract under test: one add/install/remove command is one
transaction.  On failure the DB and the active profile generation are
restored to the pre-command state; generations created by the failed
command are removed; store paths are NEVER deleted (gc reclaims them).
Crash torn states are reconciled by `verify --repair`.

Offline, tmpdir L2PM_ROOT sandboxes only; stdlib unittest conventions match
the other test_l2pm_* suites.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parent.parent
EPOCH = 315532800


class L2pmTxnBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bin = REPO / "l2pm" / "l2pm"
        subprocess.run(["make", "-C", str(REPO / "l2pm")], check=True,
                       capture_output=True)

    def setUp(self):
        self.scratch = Path(tempfile.mkdtemp(prefix="l2pm-txn-"))
        self.addCleanup(shutil.rmtree, self.scratch, True)
        self.root = self.scratch / "root"
        self.recipes = self.scratch / "recipes"
        self.recipes.mkdir()
        self.env = dict(os.environ,
                        L2PM_ROOT=str(self.root),
                        L2PM_RECIPES=str(self.recipes),
                        SOURCE_DATE_EPOCH=str(EPOCH))

    def l2pm(self, *args, ok=True):
        r = subprocess.run([str(self.bin), *args], env=self.env,
                           cwd=str(self.scratch), capture_output=True,
                           text=True, timeout=180)
        if ok:
            self.assertEqual(r.returncode, 0,
                             f"l2pm {' '.join(args)} failed:\n{r.stdout}\n{r.stderr}")
        else:
            self.assertNotEqual(r.returncode, 0,
                                f"expected failure: {r.stdout}\n{r.stderr}")
        return r

    def write_recipe(self, name, version="1.0", deps="", install=None):
        if install is None:
            install = (f"mkdir -p \"$DESTDIR/usr/bin\" && "
                       f"printf '%s\\n' '#!/bin/sh' 'echo {name}-{version}' "
                       f"> \"$DESTDIR/usr/bin/{name}\" && "
                       f"chmod 0755 \"$DESTDIR/usr/bin/{name}\"")
        (self.recipes / f"{name}.recipe").write_text(
            f"name = {name}\nversion = {version}\nepoch = {EPOCH}\n"
            f"deps = {deps}\ninstall = {install}\n")

    def db_names(self):
        db = self.root / "l2/var/db/installed"
        if not db.exists():
            return []
        return sorted(p.name for p in db.iterdir()
                      if not p.name.endswith(".manifest"))

    def profiles_dir(self):
        return self.root / "l2/var/profiles"

    def active_link(self):
        p = self.profiles_dir() / "default"
        return os.readlink(p) if p.is_symlink() else None

    def gen_dirs(self):
        p = self.profiles_dir()
        if not p.exists():
            return []
        return sorted(e.name for e in p.iterdir()
                      if e.name.startswith("default-") and e.name.endswith("-link"))

    def chmod_profiles(self, mode):
        p = self.profiles_dir()
        p.chmod(mode)
        self.addCleanup(lambda: p.chmod(0o755) if p.exists() else None)


class TransactionRollback(L2pmTxnBase):
    def test_add_dep_failure_rolls_back(self):
        # top -> mid -> base all succeed, then dep 'bad' fails to build.
        # Nothing may remain installed afterwards.
        self.write_recipe("base")
        self.write_recipe("mid", deps="base")
        self.write_recipe("bad", install="false")
        self.write_recipe("top", deps="mid bad")
        r = self.l2pm("add", "top", ok=False)
        self.assertIn("transaction failed; restored", r.stderr)
        self.assertEqual(self.db_names(), [])
        self.assertEqual(self.gen_dirs(), [])
        self.assertIsNone(self.active_link())
        self.l2pm("verify")

    def test_add_partial_failure_restores_previous_generation(self):
        self.write_recipe("keep")
        self.l2pm("add", "keep")
        self.assertEqual(self.active_link(), "default-1-link")
        self.write_recipe("extra")
        self.write_recipe("bad", install="false")
        self.write_recipe("top", deps="extra bad")
        r = self.l2pm("add", "top", ok=False)
        self.assertIn("transaction failed; restored", r.stderr)
        self.assertEqual(self.db_names(), ["keep"])
        self.assertEqual(self.active_link(), "default-1-link")
        self.assertEqual(self.gen_dirs(), ["default-1-link"])
        self.l2pm("verify")
        # the pre-existing package still works through the profile
        out = subprocess.check_output(
            [str(self.root / "l2/var/profiles/default/usr/bin/keep")],
            text=True).strip()
        self.assertEqual(out, "keep-1.0")

    def test_install_multiple_archives_atomic(self):
        self.write_recipe("oka")
        self.write_recipe("okb")
        self.l2pm("build", "oka")
        self.l2pm("build", "okb")
        a = self.root / "l2/cache/oka-1.0.l2p"
        b = self.root / "l2/cache/okb-1.0.l2p"
        bad = self.scratch / "bad.l2p"
        bad.write_bytes(b"NOT A TAR ARCHIVE")
        r = self.l2pm("install", str(a), str(b), str(bad), ok=False)
        self.assertIn("transaction failed; restored", r.stderr)
        self.assertEqual(self.db_names(), [])
        self.assertEqual(self.gen_dirs(), [])
        self.l2pm("verify")

    def test_install_multiple_archives_success(self):
        self.write_recipe("oka")
        self.write_recipe("okb")
        self.l2pm("build", "oka")
        self.l2pm("build", "okb")
        a = self.root / "l2/cache/oka-1.0.l2p"
        b = self.root / "l2/cache/okb-1.0.l2p"
        r = self.l2pm("install", str(a), str(b))
        self.assertIn("installed oka 1.0", r.stdout)
        self.assertIn("installed okb 1.0", r.stdout)
        self.assertEqual(self.db_names(), ["oka", "okb"])
        self.l2pm("verify")

    def test_install_single_failure_leaves_no_db_entry(self):
        # Force rebuild_profile to fail (read-only profiles dir) AFTER the
        # DB entry was written: the transaction must undo the DB write.
        self.l2pm("selftest")  # creates the layout under L2PM_ROOT
        self.write_recipe("solo")
        self.l2pm("build", "solo")
        pkg = self.root / "l2/cache/solo-1.0.l2p"
        self.chmod_profiles(0o555)
        self.l2pm("install", str(pkg), ok=False)
        self.assertEqual(self.db_names(), [])
        self.l2pm("verify")

    def test_remove_multiple_atomic_and_set_guard(self):
        # c depends on b, b depends on a: removing {a,b,c} together is
        # allowed (the set satisfies its own reverse deps).
        self.write_recipe("ra")
        self.write_recipe("rb", deps="ra")
        self.write_recipe("rc", deps="rb")
        self.l2pm("add", "rc")
        self.assertEqual(self.db_names(), ["ra", "rb", "rc"])
        self.l2pm("remove", "ra", "rb", "rc")
        self.assertEqual(self.db_names(), [])
        self.l2pm("verify")

    def test_remove_multi_blocked_by_external_rdep(self):
        self.write_recipe("ra")
        self.write_recipe("rb", deps="ra")
        self.write_recipe("rc", deps="rb")
        self.l2pm("add", "rc")
        r = self.l2pm("remove", "ra", "rb", ok=False)
        self.assertIn("required by: rc", r.stderr)
        self.assertEqual(self.db_names(), ["ra", "rb", "rc"])
        self.l2pm("verify")

    def test_remove_failure_restores_db(self):
        self.write_recipe("solo")
        self.l2pm("add", "solo")
        link = self.active_link()
        self.chmod_profiles(0o555)
        r = self.l2pm("remove", "solo", ok=False)
        self.assertIn("transaction failed; restored", r.stderr)
        self.assertEqual(self.db_names(), ["solo"])
        self.assertEqual(self.active_link(), link)
        self.l2pm("verify")


class TornStateRepair(L2pmTxnBase):
    @unittest.skipUnless(shutil.which("strace"), "needs strace syscall injection")
    def test_interrupted_commands_preserve_live_stores(self):
        def interrupt(call, nth, *args):
            trace = self.scratch / "trace"
            proc = subprocess.run(["strace", "-o", str(trace), "-e", f"inject={call}:signal=SIGKILL:when={nth}",
                                   str(self.bin), *args], env=self.env, capture_output=True, timeout=60)
            self.assertEqual(proc.returncode, -9, trace.read_text()[-1500:])

        self.write_recipe("keep")
        self.l2pm("add", "keep")
        self.write_recipe("app")
        self.l2pm("build", "app")
        archive = str(self.root / "l2/cache/app-1.0.l2p")
        interrupt("rename", 2, "install", archive)
        self.l2pm("verify", "--repair")
        self.l2pm("install", archive)
        interrupt("unlink", 2, "remove", "app")
        self.l2pm("verify", "--repair")
        self.assertIn("app", self.db_names())
        self.write_recipe("app", version="2.0")
        self.l2pm("add", "app")
        interrupt("rename", 2, "rollback", "1")
        intent = self.root / "l2/var/rollback-intent"
        self.assertTrue(intent.is_file())
        retry = subprocess.run(["strace", "-o", str(self.scratch / "retry.trace"),
                                "-e", "inject=symlink:error=EIO:when=1", str(self.bin),
                                "verify", "--repair"], env=self.env, capture_output=True, timeout=60)
        self.assertNotEqual(retry.returncode, 0)
        self.assertTrue(intent.is_file(), "failed recovery discarded rollback target")
        self.l2pm("verify", "--repair")
        self.assertEqual(self.active_link(), "default-1-link")
        self.assertEqual(self.db_names(), ["keep"])
        orphan = self.root / "l2/store/orphan"
        orphan.mkdir()
        (orphan / "unused").touch()
        interrupt("unlink", 1, "gc", "--prune-generations")
        self.l2pm("gc", "--prune-generations")
        self.l2pm("verify")
        active = self.profiles_dir() / "default/usr/bin/keep"
        self.assertEqual(subprocess.check_output([active], text=True).strip(), "keep-1.0")


    def test_torn_install_db_ahead_of_profile(self):
        # Simulate a crash after db_write but before rebuild_profile:
        # DB has b, the profile symlink still points at generation 1.
        self.write_recipe("ta")
        self.write_recipe("tb")
        self.l2pm("add", "ta")
        self.l2pm("add", "tb")
        p = self.profiles_dir() / "default"
        p.unlink()
        os.symlink("default-1-link", p)
        r = self.l2pm("verify", ok=False)
        self.assertIn("divergence", r.stdout)
        self.l2pm("verify", "--repair")
        self.assertEqual(self.active_link(), "default-3-link")
        self.assertTrue((self.profiles_dir() / "default/usr/bin/tb").exists())
        self.l2pm("verify")

    def test_torn_remove_profile_ahead_of_db(self):
        # Simulate a crash after the DB unlink but before rebuild_profile.
        # Repair is fail-safe: the active generation still lists tb, so the
        # DB entry is reconstructed from it (a package present in the active
        # generation is never silently dropped).
        self.write_recipe("ta")
        self.write_recipe("tb")
        self.l2pm("add", "ta")
        self.l2pm("add", "tb")
        db = self.root / "l2/var/db/installed"
        (db / "tb").unlink()
        (db / "tb.manifest").unlink()
        stores_before = sorted(e.name for e in (self.root / "l2/store").iterdir())
        r = self.l2pm("verify", ok=False)
        self.assertIn("divergence", r.stdout)
        self.l2pm("verify", "--repair")
        self.assertIn("tb", self.db_names())
        self.assertIn("version=1.0", self.l2pm("info", "tb").stdout)
        self.assertTrue((self.profiles_dir() / "default/usr/bin/tb").exists())
        self.l2pm("verify")
        # repair must NEVER delete store data (gc's job)
        stores_after = sorted(e.name for e in (self.root / "l2/store").iterdir())
        self.assertEqual(stores_before, stores_after)

    def test_torn_rollback_completes_from_intent(self):
        # Simulate a crash after the symlink switch but before the DB sync:
        # the intent marker lets verify --repair complete the rollback.
        self.write_recipe("app", version="1.0")
        self.l2pm("add", "app")
        self.write_recipe("app", version="2.0")
        self.l2pm("add", "app")
        p = self.profiles_dir() / "default"
        p.unlink()
        os.symlink("default-1-link", p)
        (self.root / "l2/var/rollback-intent").write_text(
            "profile=default\ngen=1\n")
        self.l2pm("verify", "--repair")
        self.assertEqual(self.active_link(), "default-1-link")
        self.assertIn("version=1.0", self.l2pm("info", "app").stdout)
        self.assertFalse((self.root / "l2/var/rollback-intent").exists())
        self.l2pm("verify")

    def test_torn_switch_without_marker_reconciles_to_db(self):
        # Without an intent marker the DB is the record of truth: repair
        # rebuilds the profile from it (documented contract).
        self.write_recipe("app", version="1.0")
        self.l2pm("add", "app")
        self.write_recipe("app", version="2.0")
        self.l2pm("add", "app")
        p = self.profiles_dir() / "default"
        p.unlink()
        os.symlink("default-1-link", p)
        self.l2pm("verify", "--repair")
        self.assertEqual(self.active_link(), "default-3-link")
        self.assertIn("version=2.0", self.l2pm("info", "app").stdout)
        self.l2pm("verify")

    def test_repair_rewrites_missing_manifest(self):
        self.write_recipe("ta")
        self.l2pm("add", "ta")
        mp = self.root / "l2/var/db/installed/ta.manifest"
        mp.unlink()
        self.l2pm("verify", "--repair")
        self.assertTrue(mp.exists())
        store = next((self.root / "l2/store").iterdir())
        import hashlib
        h = hashlib.sha256((store / "usr/bin/ta").read_bytes()).hexdigest()
        self.assertIn(f"F\tusr/bin/ta\t{h}", mp.read_text())
        self.l2pm("verify")

    def test_repair_refuses_to_launder_tampered_store(self):
        self.write_recipe("ta")
        self.l2pm("add", "ta")
        store = next((self.root / "l2/store").iterdir())
        target = store / "usr/bin/ta"
        target.write_bytes(b"corrupted\n")
        mp = self.root / "l2/var/db/installed/ta.manifest"
        before = mp.read_bytes()
        r = self.l2pm("verify", "--repair", ok=False)
        self.assertIn("store integrity check failed", r.stdout)
        self.assertEqual(mp.read_bytes(), before)  # never rewrite over tamper
        self.assertEqual(target.read_bytes(), b"corrupted\n")  # store untouched

    def test_repair_removes_stale_stage_artifacts(self):
        self.write_recipe("ta")
        self.l2pm("add", "ta")
        p = self.profiles_dir()
        stale = p / ".default-9-tmp-99999"
        (stale / "usr").mkdir(parents=True)
        (stale / "usr/f").write_text("x\n")
        lnk = p / ".default.link-99999"
        os.symlink("default-1-link", lnk)
        self.l2pm("verify", "--repair")
        self.assertFalse(stale.exists())
        self.assertFalse(lnk.is_symlink())
        self.assertEqual(self.gen_dirs(), ["default-1-link"])
        self.l2pm("verify")

    def test_successful_rollback_leaves_no_intent_marker(self):
        self.write_recipe("app", version="1.0")
        self.l2pm("add", "app")
        self.write_recipe("app", version="2.0")
        self.l2pm("add", "app")
        self.l2pm("rollback")
        self.assertEqual(self.active_link(), "default-1-link")
        self.assertFalse((self.root / "l2/var/rollback-intent").exists())
        self.l2pm("verify")


class SolverEdges(L2pmTxnBase):
    def test_self_dependency_rejected(self):
        self.write_recipe("selfdep", deps="selfdep")
        r = self.l2pm("add", "selfdep", ok=False)
        self.assertIn("cycle", r.stderr)
        self.assertEqual(self.db_names(), [])

    def test_three_way_cycle_nothing_installed(self):
        self.write_recipe("cya", deps="cyb")
        self.write_recipe("cyb", deps="cyc")
        self.write_recipe("cyc", deps="cya")
        r = self.l2pm("add", "cya", ok=False)
        self.assertIn("cycle", r.stderr)
        self.assertEqual(self.db_names(), [])
        self.assertEqual(self.gen_dirs(), [])

    def test_diamond_dep_built_once(self):
        counter = self.scratch / "build-count"
        self.write_recipe("dd", install=(
            f"mkdir -p \"$DESTDIR/usr/bin\" && printf 'd\\n' > \"$DESTDIR/usr/bin/dd\" "
            f"&& printf 'built\\n' >> '{counter}'"))
        self.write_recipe("db", deps="dd")
        self.write_recipe("dc", deps="dd")
        self.write_recipe("da", deps="db dc")
        self.l2pm("add", "da")
        self.assertEqual(counter.read_text().count("built"), 1)
        self.assertEqual(self.db_names(), ["da", "db", "dc", "dd"])
        self.l2pm("verify")

    def test_depth_limit_exceeded(self):
        # 34 nodes: recursion depth 33 exceeds the limit of 32.
        for i in range(34):
            deps = f"d{i + 1:02d}" if i < 33 else ""
            self.write_recipe(f"d{i:02d}", deps=deps)
        r = self.l2pm("add", "d00", ok=False)
        self.assertIn("depth limit", r.stderr)
        self.assertEqual(self.db_names(), [])

    def test_depth_at_limit_succeeds(self):
        # 33 nodes: maximum depth 32 is still allowed.
        for i in range(33):
            deps = f"d{i + 1:02d}" if i < 32 else ""
            self.write_recipe(f"d{i:02d}", deps=deps)
        self.l2pm("add", "d00")
        self.assertEqual(len(self.db_names()), 33)
        self.l2pm("verify")

    def test_version_conflict_keeps_installed_and_warns(self):
        # Name-only deps: an installed dependency is reused as-is, never
        # implicitly up/downgraded; a differing local recipe version warns.
        self.write_recipe("vv", version="1.0")
        self.l2pm("add", "vv")
        self.write_recipe("vv", version="2.0")
        self.write_recipe("user", deps="vv")
        r = self.l2pm("add", "user")
        self.assertIn("keeping installed version", r.stderr)
        self.assertIn("version=1.0", self.l2pm("info", "vv").stdout)
        self.assertEqual(self.db_names(), ["user", "vv"])
        self.l2pm("verify")

    def test_missing_dep_error_and_rollback(self):
        self.write_recipe("needy", deps="ghost")
        r = self.l2pm("add", "needy", ok=False)
        self.assertIn("recipe not found: ghost", r.stderr)
        self.assertEqual(self.db_names(), [])
        self.assertEqual(self.gen_dirs(), [])


class GcSafety(L2pmTxnBase):
    """P1: gc liveness comes from DB AND generation metadata; fail-safe on
    unreadable state; default keeps rollback history (--prune-generations
    opts into the old destructive behavior)."""

    def test_gc_refuses_on_corrupt_db_entry(self):
        self.write_recipe("ta")
        self.l2pm("add", "ta")
        store = next((self.root / "l2/store").iterdir())
        db = self.root / "l2/var/db/installed/ta"
        db.write_text("name=ta\nversion=1.0\n")  # store= line lost
        r = self.l2pm("gc", ok=False)
        self.assertIn("refusing", r.stderr)
        self.assertTrue(store.exists(), "gc deleted a live store path!")

    def test_gc_refuses_on_unparseable_generation(self):
        self.write_recipe("ta")
        self.l2pm("add", "ta")
        store = next((self.root / "l2/store").iterdir())
        gen = self.profiles_dir() / "default-1-link/.l2pm-gen"
        gen.write_text("garbage without tabs\n")
        r = self.l2pm("gc", ok=False)
        self.assertIn("refusing", r.stderr)
        self.assertTrue(store.exists())

    def test_gc_keeps_store_of_torn_remove(self):
        self.write_recipe("ta")
        self.write_recipe("tb")
        self.l2pm("add", "ta")
        self.l2pm("add", "tb")
        db = self.root / "l2/var/db/installed"
        (db / "tb").unlink()
        (db / "tb.manifest").unlink()
        stores = {e.name for e in (self.root / "l2/store").iterdir()}
        self.l2pm("gc")
        after = {e.name for e in (self.root / "l2/store").iterdir()}
        self.assertEqual(stores, after, "gc deleted a store still in the active generation")

    def test_gc_safe_on_torn_rollback(self):
        self.write_recipe("app", version="1.0")
        self.l2pm("add", "app")
        self.write_recipe("app", version="2.0")
        self.l2pm("add", "app")
        p = self.profiles_dir() / "default"
        p.unlink()
        os.symlink("default-1-link", p)  # switched, DB not yet re-synced
        stores = {e.name for e in (self.root / "l2/store").iterdir()}
        self.assertEqual(len(stores), 2)
        self.l2pm("gc")
        after = {e.name for e in (self.root / "l2/store").iterdir()}
        self.assertEqual(stores, after)

    def test_gc_default_preserves_rollback(self):
        self.write_recipe("ta")
        self.write_recipe("tb")
        self.l2pm("add", "ta")
        self.l2pm("add", "tb")
        self.l2pm("gc")  # default: keep last 2 generations
        self.l2pm("rollback")
        self.assertEqual(self.active_link(), "default-1-link")
        self.assertTrue((self.profiles_dir() / "default/usr/bin/ta").exists())
        self.assertFalse((self.profiles_dir() / "default/usr/bin/tb").exists())
        self.l2pm("verify")

    def test_gc_prune_generations_old_behavior(self):
        self.write_recipe("ta")
        self.write_recipe("tb")
        self.l2pm("add", "ta")
        self.l2pm("add", "tb")
        self.l2pm("remove", "tb")  # gen3 active, DB={ta}
        self.l2pm("gc", "--prune-generations")
        self.assertEqual(self.gen_dirs(), ["default-3-link"])
        # tb's store is unreferenced once history is pruned -> reclaimed
        self.assertEqual(len(list((self.root / "l2/store").iterdir())), 1)
        self.l2pm("rollback", ok=False)  # history intentionally gone


class Integrity(L2pmTxnBase):
    def test_reinstall_checks_cached_store_against_archive(self):
        self.write_recipe("app")
        self.l2pm("add", "app")
        pkg = str(self.root / "l2/cache/app-1.0.l2p")
        self.l2pm("install", pkg)
        program = self.profiles_dir() / "default/usr/bin/app"
        program.write_text("damaged file")
        manifest = self.root / "l2/var/db/installed/app.manifest"
        original = manifest.read_bytes()
        self.l2pm("install", pkg, ok=False)
        self.assertEqual(manifest.read_bytes(), original)
        self.l2pm("verify", ok=False)


    """P3: missing manifest never silently passes; verify --repair rebuilds
    DB entries from the active generation and manifests from the store."""

    def test_verify_fails_on_missing_manifest(self):
        self.write_recipe("ta")
        self.l2pm("add", "ta")
        (self.root / "l2/var/db/installed/ta.manifest").unlink()
        r = self.l2pm("verify", ok=False)
        self.assertIn("manifest missing", r.stdout)

    def test_repair_reconstructs_corrupt_db_entry(self):
        self.write_recipe("ta")
        self.l2pm("add", "ta")
        db = self.root / "l2/var/db/installed/ta"
        db.write_text("garbage\n")
        self.l2pm("verify", ok=False)
        self.l2pm("verify", "--repair")
        self.assertIn("version=1.0", self.l2pm("info", "ta").stdout)
        self.l2pm("verify")

    def test_repair_does_not_resurrect_orphan_stores(self):
        # An aborted transaction leaves orphan store paths; repair must not
        # adopt them into the DB (they are gc's to reclaim).
        self.write_recipe("base")
        self.write_recipe("bad", install="false")
        self.write_recipe("top", deps="base bad")
        self.l2pm("add", "top", ok=False)
        self.assertEqual(self.db_names(), [])
        self.assertTrue(list((self.root / "l2/store").iterdir()))  # orphan kept
        r = self.l2pm("verify", "--repair")
        self.assertIn("unreferenced store", r.stdout)
        self.assertEqual(self.db_names(), [])
        self.l2pm("gc")
        self.assertEqual(list((self.root / "l2/store").iterdir()), [])


class ProfileConflicts(L2pmTxnBase):
    """P5: file conflicts between packages in the profile are explicit."""

    def test_link_tree_conflict_warns(self):
        shared = ("mkdir -p \"$DESTDIR/usr/bin\" && "
                  "printf '%s\\n' '#!/bin/sh' 'echo SHARED-VERSION' "
                  "> \"$DESTDIR/usr/bin/shared\" && "
                  "chmod 0755 \"$DESTDIR/usr/bin/shared\"")
        self.write_recipe("p1", install=shared.replace("SHARED-VERSION", "one"))
        self.write_recipe("p2", install=shared.replace("SHARED-VERSION", "two"))
        self.l2pm("add", "p1")
        r = self.l2pm("add", "p2")
        self.assertIn("conflict", r.stderr)
        self.assertIn("usr/bin/shared", r.stderr)
        self.l2pm("verify")
        # documented choice: last-wins, but never silent
        out = subprocess.check_output(
            [str(self.profiles_dir() / "default/usr/bin/shared")],
            text=True).strip()
        self.assertIn(out, ("one", "two"))


if __name__ == "__main__":
    unittest.main()
