"""Unit tests for tools/patch-kernel-ram.py (M11)."""
import ast
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parent.parent
PATCHER = REPO / "tools" / "patch-kernel-ram.py"


def load_patcher_module():
    spec = importlib.util.spec_from_file_location("patch_kernel_ram", PATCHER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def extract_patch_specs():
    """Parse every patch_file(kdir / rel, [(label, old, new), ...]) call."""
    tree = ast.parse(PATCHER.read_text(encoding="utf-8"))
    specs = []
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "patch_file"
            and len(node.args) == 2
        ):
            # First arg is `kdir / "rel/path"`
            rel = ast.literal_eval(node.args[0].right)
            hunks = ast.literal_eval(node.args[1])
            specs.append((rel, hunks))
    return specs


def build_synthetic_kernel_tree(root: Path):
    specs = extract_patch_specs()
    originals = {}
    for rel, hunks in specs:
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        content = "\n/* sep */\n".join(old for _, old, _ in hunks) + "\n"
        p.write_text(content, encoding="utf-8")
        originals[rel] = content
    return originals


class TestPatchKernelRam(unittest.TestCase):
    def test_check_mode_validates_without_modifying_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            kdir = Path(tmp)
            originals = build_synthetic_kernel_tree(kdir)
            r = subprocess.run(
                ["python3", str(PATCHER), "--check", str(kdir)],
                capture_output=True, text=True, timeout=15,
            )
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("check ok", r.stdout)
            for rel, orig in originals.items():
                self.assertEqual((kdir / rel).read_text(encoding="utf-8"), orig)

    def test_apply_patches_expected_hardening_hunks_atomically(self):
        with tempfile.TemporaryDirectory() as tmp:
            kdir = Path(tmp)
            build_synthetic_kernel_tree(kdir)
            page_alloc = kdir / "mm/page_alloc.c"
            page_alloc.chmod(0o640)
            r = subprocess.run(
                ["python3", str(PATCHER), str(kdir)],
                capture_output=True, text=True, timeout=15,
            )
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("patched", r.stdout)
            # File mode preserved and no leftover temp files.
            self.assertEqual(page_alloc.stat().st_mode & 0o777, 0o640)
            self.assertEqual(list(kdir.rglob("*.tmp")), [])

            pa_text = page_alloc.read_text(encoding="utf-8")
            # Race-free PCP drain: drain_all_pages called after mutex_unlock, no unlocked global read.
            self.assertIn("mutex_unlock(&pcp_batch_high_lock);\n\tdrain_all_pages(NULL);\n\treturn 0;", pa_text)
            self.assertNotIn("percpu_pagelist_high_fraction != old_percpu_pagelist_high_fraction", pa_text)
            # OOM RCU barrier gated on __GFP_FS.
            self.assertIn("IS_ENABLED(CONFIG_BASE_SMALL) && (gfp_mask & __GFP_FS)", pa_text)
            # Realmode low-1M skip gated on <64 MiB.
            rm_text = (kdir / "arch/x86/realmode/init.c").read_text(encoding="utf-8")
            self.assertIn("memblock_phys_mem_size() >= SZ_64M", rm_text)

    def test_late_mismatch_leaves_all_files_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            kdir = Path(tmp)
            originals = build_synthetic_kernel_tree(kdir)
            (kdir / "arch/x86/realmode/init.c").write_text("/* mismatch */\n", encoding="utf-8")
            for extra in ([], ["--check"]):
                r = subprocess.run(
                    ["python3", str(PATCHER), *extra, str(kdir)],
                    capture_output=True, text=True, timeout=15,
                )
                self.assertNotEqual(r.returncode, 0)
                self.assertIn("realmode_keep_low_1m_small", r.stderr)
                self.assertEqual(
                    (kdir / "mm/page_alloc.c").read_text(encoding="utf-8"),
                    originals["mm/page_alloc.c"],
                )

    def test_atomic_write_preserves_original_on_replace_error(self):
        mod = load_patcher_module()
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "sample.c"
            target.write_text("original\n", encoding="utf-8")
            with mock.patch("os.replace", side_effect=OSError("disk full")):
                with self.assertRaises(OSError):
                    mod.atomic_write_text(target, "updated\n")
            self.assertEqual(target.read_text(encoding="utf-8"), "original\n")
            self.assertEqual(list(Path(tmp).glob("*.tmp")), [])

    def test_invalid_usage_and_missing_dir(self):
        r = subprocess.run(["python3", str(PATCHER)], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("usage:", r.stderr)
        with tempfile.TemporaryDirectory() as tmp:
            r = subprocess.run(["python3", str(PATCHER), tmp], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("invalid kernel source dir", r.stderr)


if __name__ == "__main__":
    unittest.main()
