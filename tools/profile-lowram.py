#!/usr/bin/env python3
"""Measure boot stages, managed pages and reclaimable caches in a disposable BIOS VM."""
import argparse
import re
import time
from pathlib import Path

import lowram_matrix as matrix


def profile(iso, log, size):
    started = time.monotonic()
    with log.open("xb") as stream:
        guest = matrix.Guest(matrix.build_cmd(size, "bios", 2, iso=str(iso)), stream)
        try:
            for marker in (b"rcl2:", matrix.PROMPT):
                if not guest.read_until(marker, 120):
                    raise RuntimeError(f"missing boot stage {marker!r}")
                print(f"{marker.decode()} {time.monotonic() - started:.3f} s", flush=True)
            time.sleep(40)
            before = guest.meminfo()
            for cmd in ("cat /proc/zoneinfo", "dmesg"):
                ok, output = guest.probe(cmd, "L2PROFILEDONE")
                if not ok:
                    raise RuntimeError(f"probe failed: {cmd}")
                if cmd.endswith("zoneinfo"):
                    pages = re.findall(r"^\s*managed\s+(\d+)", output, re.MULTILINE)
                    if not pages or 4 * sum(map(int, pages)) != before["MemTotal"]:
                        raise RuntimeError("managed page count disagrees with x86 MemTotal")
                for line in output.splitlines():
                    if re.search(r"^\s*(managed|present|spanned)\s+\d+|Memory:|Freeing .*memory|Dentry cache hash|Inode-cache hash", line):
                        print(line)
            ok, reply = guest.probe("echo 2 > /proc/sys/vm/drop_caches && echo RECLAIM''OK", "L2DROPDONE")
            if not ok or "RECLAIMOK" not in reply:
                raise RuntimeError("cache reclaim failed")
            after = guest.meminfo()
            for label, info in (("settled", before), ("after reclaim", after)):
                if not info or not {"Slab", "MemTotal", "MemAvailable"} <= info.keys():
                    raise RuntimeError("incomplete memory profile")
                print(label, {k: info[k] for k in ("MemTotal", "MemAvailable", "Slab", "SReclaimable")},
                      "reserved KiB", size * 1024 - info["MemTotal"])
            # Charge the unused Wi-Fi client in a disposable v3 initramfs only.
            ok, reply = guest.probe(
                "du -k /bin/l2wifi; rm /bin/l2wifi && echo UNLINK''OK", "L2WIFIDONE")
            if not ok or "UNLINKOK" not in reply:
                raise RuntimeError("could not measure the Wi-Fi client")
            trimmed = guest.meminfo()
            if trimmed["Shmem"] >= after["Shmem"]:
                raise RuntimeError("unused client yielded no tmpfs reduction")
            print("wifi file charge:", reply.strip(), "Shmem KiB before/after:",
                  after["Shmem"], trimmed["Shmem"])
            guest.send("poweroff")
            if guest.p.wait(timeout=30):
                raise RuntimeError("unclean shutdown")
        finally:
            guest.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("iso", type=Path)
    ap.add_argument("log", type=Path)
    ap.add_argument("--ram", type=int, default=10)
    args = ap.parse_args()
    profile(args.iso, args.log, args.ram)
