#!/usr/bin/env python3
"""Check an identified low-RAM ISO under BIOS and UEFI using a private snapshot."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("sha256", help="expected digest from the build under review")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="l2-verify-") as temp:
        image = Path(temp) / "l2.iso"
        with args.iso.open("rb") as source:
            payload = source.read(10 * 1024**2)
        size = len(payload)
        if not 0 < size < 10 * 1024**2:
            parser.error("ISO size outside the release budget (must be nonempty and below 10 MiB)")
        digest = hashlib.sha256(payload).hexdigest()
        if digest != args.sha256:
            parser.error(f"snapshot digest mismatch: {digest}")
        image.write_bytes(payload)
        print(f"ISO {size} bytes, SHA-256 {digest}", flush=True)
        subprocess.run(["make", "check"], cwd=root, check=True)
        for firmware in (["--ram", "10"], ["--uefi", "--ram", "128"]):
            subprocess.run([sys.executable, root / "tools/ramtest.py", "--iso", image,
                            "--pid1-budget", "50", "--budget", "10240", "--settle", "40",
                            *firmware], check=True)


if __name__ == "__main__":
    main()
