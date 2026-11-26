#!/usr/bin/env python3
"""Check a pinned core ISO and print the single-profile 72-hour test command."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shlex
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("sha256")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    iso = args.iso.resolve(strict=True)
    with iso.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != args.sha256 or not 0 < iso.stat().st_size < 10 * 1024**2:
        parser.error("ISO hash or size does not match the release gate")

    harness = Path(__file__).resolve().parents[4] / "tools/test-stability.py"
    spec = importlib.util.spec_from_file_location("l2_stability", harness)
    stability = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stability)
    options = [
        "--iso", str(iso), "--ram", "256", "--min-ram", "256",
        "--boots", "100", "--soak-hours", "72", "--sample-interval", "60",
        "--output", str(args.output.resolve()),
    ]
    ready = stability.parse_args(options)
    print(f"SHA-256 {digest}. QEMU acceleration {ready.accel}.", file=sys.stderr)
    print(shlex.join([sys.executable, str(harness), *options]))


if __name__ == "__main__":
    main()
