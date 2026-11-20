"""Regression tests for the l2pm 4-step recipe pipeline.

Covers: git package sources with commit pinning, phased compile/install,
legacy build= compatibility, remote recipe repositories (git and raw-URL
transports), the trust gate, offline cache reuse and local precedence.

All git fixtures are LOCAL repos in scratch dirs; everything is offline.
Skips gracefully when git is unavailable.  Wired into `make check`.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parent.parent
EPOCH = 315532800
HAVE_GIT = shutil.which("git") is not None


def run(cmd, cwd=None, env=None, ok=True, stdin=None):
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True,
                       timeout=180, stdin=stdin)
    if ok:
        assert r.returncode == 0, f"{cmd} failed:\n{r.stdout}\n{r.stderr}"
    return r


def git_env(home):
    """Hermetic git environment: no system/global config (insteadOf rules,
    gpgsign, templates ... must not leak into the fixtures)."""
    return dict(os.environ, GIT_CONFIG_NOSYSTEM="1",
                GIT_CONFIG_GLOBAL=os.devnull, HOME=str(home))


def git_fixture(path, files, message="init", home=None):
    """Create a local git repo with the given {name: content} files."""
    env = git_env(home or path.parent / "git-home")
    Path(env["HOME"]).mkdir(parents=True, exist_ok=True)
    path.mkdir(parents=True, exist_ok=True)
    run(["git", "init", "-q", "-b", "main", str(path)], env=env)
    run(["git", "config", "user.email", "test@l2.local"], cwd=path, env=env)
    run(["git", "config", "user.name", "L2 Test"], cwd=path, env=env)
    for name, content in files.items():
        p = path / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
    run(["git", "add", "-A"], cwd=path, env=env)
    run(["git", "commit", "-qm", message], cwd=path, env=env)
    return run(["git", "rev-parse", "HEAD"], cwd=path, env=env).stdout.strip()


class Pipeline(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bin = REPO / "l2pm" / "l2pm"
        if not cls.bin.exists():
            subprocess.run(["make", "-C", str(REPO / "l2pm")], check=True,
                           capture_output=True)
        if not cls.bin.exists():
            raise unittest.SkipTest("l2pm binary unavailable")
        cls.musl_cc = REPO / "build" / "bin" / "musl-gcc"

    def setUp(self):
        self.scratch = Path(tempfile.mkdtemp(prefix="l2pm-pipe-"))
        self.addCleanup(shutil.rmtree, self.scratch, True)
        self.root = self.scratch / "root"
        self.env = dict(os.environ, L2PM_ROOT=str(self.root),
                        L2PM_RECIPES=str(self.scratch / "recipes"),
                        SOURCE_DATE_EPOCH=str(EPOCH))
        if self.musl_cc.exists():
            self.env["CC"] = str(self.musl_cc)
        (self.scratch / "recipes").mkdir()
        # keep git quiet and deterministic everywhere (fixtures and the
        # git calls l2pm itself makes)
        self.env["GIT_CONFIG_NOSYSTEM"] = "1"
        self.env["GIT_CONFIG_GLOBAL"] = os.devnull
        self.env["HOME"] = str(self.scratch / "home")
        (self.scratch / "home").mkdir()

    def l2pm(self, *args, ok=True, extra_env=None, stdin=subprocess.DEVNULL):
        env = dict(self.env)
        if extra_env:
            env.update(extra_env)
        r = subprocess.run([str(self.bin), *args], env=env, cwd=str(self.scratch),
                           capture_output=True, text=True, timeout=180, stdin=stdin)
        if ok:
            self.assertEqual(r.returncode, 0,
                             f"l2pm {' '.join(args)} failed:\n{r.stdout}\n{r.stderr}")
        else:
            self.assertNotEqual(r.returncode, 0,
                                f"expected failure: {r.stdout}\n{r.stderr}")
        return r

    def write_recipe(self, name, text):
        p = self.scratch / "recipes" / f"{name}.recipe"
        p.write_text(text)
        return p

    def store_entries(self):
        store = self.root / "l2" / "store"
        return list(store.iterdir()) if store.exists() else []

    # ------------------------------------------------- git package sources
    @unittest.skipUnless(HAVE_GIT, "git not available")
    def test_git_source_pinned_build(self):
        src = self.scratch / "src-repo"
        rev = git_fixture(src, {"hello.sh": "#!/bin/sh\necho hello\n"})
        self.write_recipe("gitsrc", f"""name = gitsrc
version = 1.0
epoch = {EPOCH}
repo = {src}
ref = {rev}
install = mkdir -p "$DESTDIR/usr/bin" && install -m 0755 hello.sh "$DESTDIR/usr/bin/hello"
""")
        r = self.l2pm("build", "gitsrc")
        self.assertIn("[1/4] repo: git", r.stderr)
        self.assertIn("[2/4] fetch", r.stderr)
        self.assertIn("[4/4] install", r.stderr)
        entries = self.store_entries()
        self.assertEqual(len(entries), 0)  # build only, not installed
        pkg = self.root / "l2" / "cache" / "gitsrc-1.0.l2p"
        self.assertTrue(pkg.exists())
        self.l2pm("install", str(pkg))
        sd = self.store_entries()[0]
        self.assertEqual((sd / "usr/bin/hello").read_text(), "#!/bin/sh\necho hello\n")
        # .git must not leak into the package; mtimes normalized to epoch
        self.assertFalse((sd / ".git").exists())
        self.assertEqual(int((sd / "usr/bin/hello").stat().st_mtime), EPOCH)

    @unittest.skipUnless(HAVE_GIT, "git not available")
    def test_git_source_reproducible(self):
        src = self.scratch / "src-repo"
        rev = git_fixture(src, {"hello.sh": "#!/bin/sh\necho hi\n",
                                "sub/data.txt": "payload\n"})
        self.write_recipe("gitrep", f"""name = gitrep
version = 1.0
epoch = {EPOCH}
repo = {src}
ref = {rev}
install = mkdir -p "$DESTDIR/usr/share" && cp -a hello.sh sub "$DESTDIR/usr/share/"
""")
        r = self.l2pm("build", "gitrep", "--check")
        self.assertIn("reproducible: yes", r.stdout)

    @unittest.skipUnless(HAVE_GIT, "git not available")
    def test_git_source_compile_and_install(self):
        src = self.scratch / "src-repo"
        rev = git_fixture(src, {"hello.c": '#include <stdio.h>\n'
                                'int main(void){puts("compiled");return 0;}\n'})
        self.write_recipe("gitcc", f"""name = gitcc
version = 1.0
epoch = {EPOCH}
repo = {src}
ref = {rev}
compile = ${{CC:-cc}} -O2 -o hello hello.c
install = mkdir -p "$DESTDIR/usr/bin" && install -m 0755 hello "$DESTDIR/usr/bin/hello"
""")
        r = self.l2pm("build", "gitcc")
        self.assertIn("[3/4] compile", r.stderr)
        self.assertIn("[4/4] install: staging", r.stderr)
        pkg = self.root / "l2" / "cache" / "gitcc-1.0.l2p"
        self.l2pm("install", str(pkg))
        binary = self.store_entries()[0] / "usr/bin/hello"
        out = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(out.stdout.strip(), "compiled")

    @unittest.skipUnless(HAVE_GIT, "git not available")
    def test_git_source_tag_ref_warns(self):
        src = self.scratch / "src-repo"
        git_fixture(src, {"hello.sh": "x\n"})
        run(["git", "tag", "v1"], cwd=src, env=self.env)
        self.write_recipe("gittag", f"""name = gittag
version = 1.0
epoch = {EPOCH}
repo = {src}
ref = v1
install = mkdir -p "$DESTDIR" && cp hello.sh "$DESTDIR/"
""")
        r = self.l2pm("build", "gittag")
        self.assertIn("NOT reproducible", r.stderr)

    @unittest.skipUnless(HAVE_GIT, "git not available")
    def test_git_source_missing_ref_rejected(self):
        self.write_recipe("gitnoref", f"""name = gitnoref
version = 1.0
repo = {self.scratch}/whatever
install = true
""")
        r = self.l2pm("build", "gitnoref", ok=False)
        self.assertIn("needs ref=", r.stderr)

    @unittest.skipUnless(HAVE_GIT, "git not available")
    def test_git_source_unknown_ref_rejected(self):
        src = self.scratch / "src-repo"
        git_fixture(src, {"hello.sh": "x\n"})
        self.write_recipe("gitbadref", f"""name = gitbadref
version = 1.0
epoch = {EPOCH}
repo = {src}
ref = {"deadbeef" * 5}
install = true
""")
        r = self.l2pm("build", "gitbadref", ok=False)
        self.assertIn("checkout", r.stderr)

    @unittest.skipUnless(HAVE_GIT, "git not available")
    def test_git_ref_injection_rejected(self):
        src = self.scratch / "src-repo"
        git_fixture(src, {"hello.sh": "x\n"})
        canary = self.scratch / "pwned"
        for bad in (f"x;touch {canary}", "--upload-pack=touch"):
            self.write_recipe("gitevil", f"""name = gitevil
version = 1.0
epoch = {EPOCH}
repo = {src}
ref = {bad}
install = true
""")
            self.l2pm("build", "gitevil", ok=False)
            self.assertFalse(canary.exists(), "ref injection executed!")

    # ------------------------------------------------------ recipe validation
    def test_source_and_repo_conflict(self):
        self.write_recipe("conflict", """name = conflict
version = 1.0
source = https://example.org/x.tar.gz
repo = https://example.org/x.git
ref = abc
build = true
""")
        r = self.l2pm("build", "conflict", ok=False)
        self.assertIn("not both", r.stderr)

    def test_build_and_phased_conflict(self):
        self.write_recipe("conflict2", """name = conflict2
version = 1.0
build = true
compile = true
install = true
""")
        r = self.l2pm("build", "conflict2", ok=False)
        self.assertIn("not both", r.stderr)

    def test_compile_requires_install(self):
        self.write_recipe("noinst", """name = noinst
version = 1.0
compile = true
""")
        r = self.l2pm("build", "noinst", ok=False)
        self.assertIn("requires install=", r.stderr)

    # ------------------------------------------------------- phased + legacy
    def test_phased_local_source(self):
        (self.scratch / "data.txt").write_text("local\n")
        self.write_recipe("phased", f"""name = phased
version = 1.0
epoch = {EPOCH}
source = {self.scratch}/data.txt
compile = cp data.txt compiled.txt
install = mkdir -p "$DESTDIR/usr/share" && install -m 0644 compiled.txt "$DESTDIR/usr/share/"
""")
        r = self.l2pm("build", "phased", "--check")
        self.assertIn("[3/4] compile", r.stderr)
        self.assertIn("[4/4] install: staging", r.stderr)
        self.assertIn("reproducible: yes", r.stdout)

    def test_legacy_build_still_works(self):
        (self.scratch / "legacy.sh").write_text("#!/bin/sh\nexit 0\n")
        self.write_recipe("legacy", f"""name = legacy
version = 1.0
epoch = {EPOCH}
source = {self.scratch}/legacy.sh
build = mkdir -p "$DESTDIR/usr/bin" && install -m 0755 legacy.sh "$DESTDIR/usr/bin/legacy"
""")
        r = self.l2pm("build", "legacy", "--check")
        self.assertIn("legacy compile+install", r.stderr)
        self.assertIn("reproducible: yes", r.stdout)

    def test_shipped_recipes_still_reproducible(self):
        # rcl2.recipe is excluded: it builds inside the user's rcl2/ WIP tree.
        for name in ("l2fetch", "l2install", "l2pm", "l2wifi"):
            with self.subTest(recipe=name):
                recipe = REPO / "l2pm" / "recipes" / f"{name}.recipe"
                r = self.l2pm("build", str(recipe), "--check")
                self.assertIn("reproducible: yes", r.stdout)

    # --------------------------------------------------- remote recipe repo
    @unittest.skipUnless(HAVE_GIT, "git not available")
    def test_recipe_repo_git_trust_flow(self):
        remote = self.scratch / "remote.git"
        recipe_text = f"""name = remotepkg
version = 1.0
epoch = {EPOCH}
install = mkdir -p "$DESTDIR/usr/bin" && printf '#!/bin/sh\\necho remote\\n' > "$DESTDIR/usr/bin/remotepkg" && chmod 0755 "$DESTDIR/usr/bin/remotepkg"
"""
        git_fixture(remote, {"remotepkg.recipe": recipe_text},
                    home=self.scratch / "home")
        repo_env = {"L2PM_RECIPE_REPO": str(remote)}

        # 1. untrusted, non-interactive: refused, nothing installed
        r = self.l2pm("add", "remotepkg", ok=False, extra_env=repo_env)
        self.assertIn("refusing to execute an untrusted remote recipe", r.stderr)
        self.assertIn("install = mkdir", r.stderr)  # recipe was shown
        self.assertEqual(self.store_entries(), [])

        # 2. --trust: installs and records the hash
        r = self.l2pm("add", "remotepkg", "--trust", extra_env=repo_env)
        self.assertIn("installed remotepkg 1.0", r.stdout)
        trusted = self.root / "l2" / "var" / "trusted-recipes"
        self.assertTrue(trusted.exists())
        self.assertEqual(len(trusted.read_text().splitlines()), 1)

        # 3. same content again: no prompt needed
        r = self.l2pm("add", "remotepkg", extra_env=repo_env)
        self.assertIn("installed remotepkg", r.stdout)

        # 4. changed remote recipe: refused again (hash changed)
        (remote / "remotepkg.recipe").write_text(recipe_text.replace("1.0", "2.0"))
        run(["git", "add", "-A"], cwd=remote, env=self.env)
        run(["git", "commit", "-qm", "bump"], cwd=remote, env=self.env)
        r = self.l2pm("add", "remotepkg", ok=False, extra_env=repo_env)
        self.assertIn("refusing", r.stderr)

    @unittest.skipUnless(HAVE_GIT, "git not available")
    def test_recipe_repo_git_pinned_ref(self):
        remote = self.scratch / "remote.git"
        recipe_text = f"""name = pinpkg
version = 1.0
epoch = {EPOCH}
install = mkdir -p "$DESTDIR" && echo one > "$DESTDIR/one"
"""
        rev1 = git_fixture(remote, {"pinpkg.recipe": recipe_text})
        (remote / "pinpkg.recipe").write_text(recipe_text.replace("one", "two"))
        run(["git", "add", "-A"], cwd=remote, env=self.env)
        run(["git", "commit", "-qm", "two"], cwd=remote, env=self.env)
        repo_env = {"L2PM_RECIPE_REPO": str(remote), "L2PM_RECIPE_REPO_REF": rev1}
        self.l2pm("add", "pinpkg", "--trust", extra_env=repo_env)
        sd = self.store_entries()[0]
        self.assertTrue((sd / "one").exists())
        self.assertFalse((sd / "two").exists())

    @unittest.skipUnless(HAVE_GIT, "git not available")
    def test_recipe_repo_git_offline_cache(self):
        remote = self.scratch / "remote.git"
        recipe_text = f"""name = offpkg
version = 1.0
epoch = {EPOCH}
install = mkdir -p "$DESTDIR" && echo cached > "$DESTDIR/f"
"""
        git_fixture(remote, {"offpkg.recipe": recipe_text})
        repo_env = {"L2PM_RECIPE_REPO": str(remote)}
        self.l2pm("add", "offpkg", "--trust", extra_env=repo_env)
        # remote disappears: the cached clone + trust store keep it working
        remote.rename(self.scratch / "remote-gone")
        r = self.l2pm("add", "offpkg", extra_env=repo_env)
        self.assertIn("installed offpkg", r.stdout)

    @unittest.skipUnless(shutil.which("curl"), "curl not available")
    def test_recipe_repo_raw_url_and_cache(self):
        raw = self.scratch / "raw"
        raw.mkdir()
        recipe_text = f"""name = rawpkg
version = 1.0
epoch = {EPOCH}
install = mkdir -p "$DESTDIR" && echo raw > "$DESTDIR/f"
"""
        (raw / "rawpkg.recipe").write_text(recipe_text)
        repo_env = {"L2PM_RECIPE_REPO": f"file://{raw}"}
        r = self.l2pm("add", "rawpkg", ok=False, extra_env=repo_env)
        self.assertIn("refusing", r.stderr)
        self.l2pm("add", "rawpkg", "--trust", extra_env=repo_env)
        # offline: cached copy + recorded trust still work
        raw.rename(self.scratch / "raw-gone")
        r = self.l2pm("add", "rawpkg", extra_env=repo_env)
        self.assertIn("installed rawpkg", r.stdout)

    @unittest.skipUnless(HAVE_GIT, "git not available")
    def test_local_recipes_take_precedence(self):
        remote = self.scratch / "remote.git"
        remote_text = f"""name = dualpkg
version = 2.0
epoch = {EPOCH}
install = mkdir -p "$DESTDIR" && echo remote > "$DESTDIR/f"
"""
        git_fixture(remote, {"dualpkg.recipe": remote_text})
        local_text = f"""name = dualpkg
version = 1.0
epoch = {EPOCH}
install = mkdir -p "$DESTDIR" && echo local > "$DESTDIR/f"
"""
        self.write_recipe("dualpkg", local_text)
        # no --trust, non-interactive: local recipe must win without a gate
        r = self.l2pm("add", "dualpkg",
                      extra_env={"L2PM_RECIPE_REPO": str(remote)})
        self.assertIn("installed dualpkg 1.0", r.stdout)
        self.assertEqual((self.store_entries()[0] / "f").read_text().strip(), "local")

    def test_bare_name_not_shadowed_by_cwd_file(self):
        # an unrelated file named like the recipe in the CWD must not win
        (self.scratch / "shadowpkg").write_text("#!/bin/sh\n# not a recipe\n")
        self.write_recipe("shadowpkg", f"""name = shadowpkg
version = 1.0
epoch = {EPOCH}
install = mkdir -p "$DESTDIR" && echo real > "$DESTDIR/f"
""")
        r = self.l2pm("add", "shadowpkg")
        self.assertIn("installed shadowpkg 1.0", r.stdout)


if __name__ == "__main__":
    unittest.main()