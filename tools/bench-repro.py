#!/usr/bin/env python3
"""Build reproducibility & benchmark regression gate for L2 (M20).

Usage:
  python3 tools/bench-repro.py --selftest
  python3 tools/bench-repro.py --check-repro [l2wifi l2gpt l2pm rcl2]
  python3 tools/bench-repro.py --check-budgets [--baseline base.json] [--json out.json]
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parent.parent
MUSL_CC = REPO / "build" / "bin" / "musl-gcc"

DEFAULT_BUDGETS = {
    "core_iso_bytes": 10 * 1024 * 1024,       # < 10 MiB
    "idle_ram_kib": 10240,                    # <= 10 MiB
    "pid1_rss_kib": 50,                       # <= 50 KiB
    "l2pm_rss_kib": 10240,                    # <= 10 MiB
    "l2pm_bin_bytes": 10 * 1024 * 1024,       # < 10 MiB
}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def compare_builds(path_a: Path, path_b: Path) -> dict:
    sha_a = sha256_file(path_a)
    sha_b = sha256_file(path_b)
    size_a = path_a.stat().st_size
    size_b = path_b.stat().st_size
    return {
        "sha256_a": sha_a,
        "sha256_b": sha_b,
        "size_a": size_a,
        "size_b": size_b,
        "identical": sha_a == sha_b and size_a == size_b,
    }


def evaluate_budgets(measured: dict, budgets: dict = None, baseline: dict = None,
                     max_regression_pct: float = 5.0) -> list[str]:
    """Return a list of violation strings (empty means PASS)."""
    limits = dict(DEFAULT_BUDGETS)
    if budgets:
        limits.update(budgets)
    violations = []
    for key, val in measured.items():
        if val is None:
            continue
        if key in limits and val > limits[key]:
            violations.append(f"{key}={val} exceeds budget {limits[key]}")
        if baseline and key in baseline and baseline[key] is not None and baseline[key] > 0:
            allowed = baseline[key] * (1.0 + max_regression_pct / 100.0)
            if val > allowed:
                violations.append(
                    f"{key}={val} regressed >{max_regression_pct:.1f}% over baseline {baseline[key]}"
                )
    return violations


def _build_component_in(comp: str, workdir: Path, cc: str) -> Path:
    env = dict(os.environ, SOURCE_DATE_EPOCH="1767225600", TZ="UTC", LC_ALL="C", LANG="C")
    if comp == "l2wifi":
        dst = workdir / "l2wifi"
        shutil.copytree(REPO / "l2wifi", dst)
        subprocess.run(["make", "-C", str(dst), "clean"], env=env, check=True, capture_output=True)
        subprocess.run(["make", "-C", str(dst), f"CC={cc}"], env=env, check=True, capture_output=True)
        return dst / "l2wifi"
    if comp == "l2pm":
        dst = workdir / "l2pm"
        shutil.copytree(REPO / "l2pm", dst)
        out = dst / "l2pm"
        subprocess.run(
            [
                cc, "-std=c11", "-Os", "-static", "-ffunction-sections", "-fdata-sections",
                "-Wl,--gc-sections", "-Wall", "-Wextra",
                f"-ffile-prefix-map={dst}=/l2/src/l2pm",
                f"-fdebug-prefix-map={dst}=/l2/src/l2pm",
                "-o", str(out), str(dst / "l2pm.c"),
            ],
            env=env, check=True, capture_output=True,
        )
        subprocess.run(["strip", "-s", str(out)], env=env, check=False, capture_output=True)
        return out
    if comp == "l2gpt":
        dst = workdir / "l2gpt"
        shutil.copytree(REPO / "l2gpt", dst)
        out = dst / "l2gpt"
        subprocess.run(
            [
                cc, "-std=c11", "-Os", "-static", "-Wall", "-Wextra",
                f"-ffile-prefix-map={dst}=/l2/src/l2gpt",
                f"-fdebug-prefix-map={dst}=/l2/src/l2gpt",
                "-o", str(out), str(dst / "l2gpt.c"),
            ],
            env=env, check=True, capture_output=True,
        )
        subprocess.run(["strip", "-s", str(out)], env=env, check=False, capture_output=True)
        return out
    if comp == "rcl2":
        dst = workdir / "rcl2"
        shutil.copytree(REPO / "rcl2", dst)
        subprocess.run(["make", "-C", str(dst), "clean"], env=env, check=True, capture_output=True)
        subprocess.run(["make", "-C", str(dst)], env=env, check=True, capture_output=True)
        return dst / "build" / "rcl2"
    raise ValueError(f"unsupported component: {comp}")


def check_repro(components: list[str], scratch_root: Path = None) -> tuple[bool, dict]:
    cc = str(MUSL_CC) if MUSL_CC.exists() else os.environ.get("CC", "cc")
    base = scratch_root or Path("/tmp/archnemesis/hydrogen")
    base.mkdir(parents=True, exist_ok=True)
    results = {}
    all_ok = True
    with tempfile.TemporaryDirectory(prefix="repro-a-", dir=base) as dir_a, \
            tempfile.TemporaryDirectory(prefix="repro-b-longer-path-", dir=base) as dir_b:
        for comp in components:
            bin_a = _build_component_in(comp, Path(dir_a), cc)
            bin_b = _build_component_in(comp, Path(dir_b), cc)
            cmp_res = compare_builds(bin_a, bin_b)
            results[comp] = cmp_res
            if not cmp_res["identical"]:
                all_ok = False
    return all_ok, results


def collect_budgets() -> dict:
    measured = {}
    iso = REPO / "build" / "out" / "l2.iso"
    if iso.is_file():
        measured["core_iso_bytes"] = iso.stat().st_size
    l2pm = REPO / "build" / "out" / "l2pm"
    if not l2pm.is_file():
        l2pm = REPO / "l2pm" / "l2pm"
    if l2pm.is_file():
        measured["l2pm_bin_bytes"] = l2pm.stat().st_size
    return measured


def selftest() -> int:
    with tempfile.TemporaryDirectory(prefix="bench-repro-self-") as tmp:
        p = Path(tmp)
        a = p / "a.bin"
        b = p / "b.bin"
        c = p / "c.bin"
        a.write_bytes(b"identical-payload")
        b.write_bytes(b"identical-payload")
        c.write_bytes(b"different-payload")
        res_eq = compare_builds(a, b)
        assert res_eq["identical"] is True
        assert res_eq["sha256_a"] == res_eq["sha256_b"]
        res_neq = compare_builds(a, c)
        assert res_neq["identical"] is False

        assert evaluate_budgets({"core_iso_bytes": 8_000_000, "pid1_rss_kib": 40}) == []
        v_budget = evaluate_budgets({"core_iso_bytes": 11 * 1024 * 1024, "pid1_rss_kib": 64})
        assert len(v_budget) == 2, v_budget
        v_reg = evaluate_budgets(
            {"idle_ram_kib": 6000},
            baseline={"idle_ram_kib": 5500},
            max_regression_pct=5.0,
        )
        assert len(v_reg) == 1, v_reg
        assert evaluate_budgets(
            {"idle_ram_kib": 5700},
            baseline={"idle_ram_kib": 5500},
            max_regression_pct=5.0,
        ) == []
    print("bench-repro selftest ok")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true", help="run offline unit checks")
    ap.add_argument(
        "--check-repro",
        nargs="*",
        metavar="COMP",
        help="double-build components from 2 distinct dirs and verify SHA-256 identity",
    )
    ap.add_argument("--check-budgets", action="store_true", help="check artifact budgets")
    ap.add_argument("--baseline", type=Path, help="baseline JSON for regression checks")
    ap.add_argument("--max-regression-pct", type=float, default=5.0)
    ap.add_argument("--json", type=Path, help="write JSON report to PATH")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    report = {}
    ok = True

    if args.check_repro is not None:
        comps = args.check_repro or ["l2wifi", "l2gpt", "l2pm"]
        repro_ok, repro_res = check_repro(comps)
        report["repro"] = repro_res
        for comp, r in repro_res.items():
            tag = "PASS" if r["identical"] else "FAIL"
            print(f"repro {comp:8s} {tag}  size={r['size_a']} B  sha256={r['sha256_a'][:16]}...")
        if not repro_ok:
            ok = False

    if args.check_budgets:
        measured = collect_budgets()
        baseline = json.loads(args.baseline.read_text()) if args.baseline else None
        violations = evaluate_budgets(
            measured, baseline=baseline, max_regression_pct=args.max_regression_pct
        )
        report["budgets"] = {"measured": measured, "violations": violations}
        for k, v in measured.items():
            print(f"budget {k}={v}")
        for v in violations:
            print(f"FAIL: {v}")
        if violations:
            ok = False

    if not report:
        ap.print_help()
        return 2

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
