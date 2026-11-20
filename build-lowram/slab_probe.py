#!/usr/bin/env python3
"""Boot the lowram ISO at 16M and dump slab composition (ground truth for
targeting cuts).  Tries /proc/slabinfo; falls back to /sys/kernel/slab sums.
Usage: slab_probe.py [ISO] [MiB]
"""
import importlib.util
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("lowram_matrix", HERE / "lowram_matrix.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

iso = sys.argv[1] if len(sys.argv) > 1 else str(HERE / "out" / "l2-lowram.iso")
size = int(sys.argv[2]) if len(sys.argv) > 2 else 16
logpath = HERE / "campaigns" / f"slabprobe-{size}M.log"
logpath.parent.mkdir(parents=True, exist_ok=True)
cmd = m.build_cmd(size, "bios", 2, iso=iso)
with open(logpath, "ab") as fh:
    g = m.Guest(cmd, fh)
    try:
        if not g.read_until(m.PROMPT, 180):
            print("FAIL: no prompt"); sys.exit(1)
        time.sleep(8)
        for c in ("cat /proc/slabinfo", "cat /proc/meminfo"):
            ok, text = g.probe(c, "L2SLABDONE", timeout=25)
            print(f"===== {c} (ok={ok}) =====")
            print(text[:6000])
        # per-cache object counts via sysfs (works even without slabinfo)
        ok, text = g.probe(
            "for d in /sys/kernel/slab/*; do o=$(cat $d/objects 2>/dev/null); "
            "s=$(cat $d/object_size 2>/dev/null); echo \"$o $s ${d##*/}\"; done "
            "| awk '{kb=$1*$2/1024; printf \"%8.0f KiB %6d obj x %5d B %s\\n\", kb,$1,$2,$3}' "
            "| sort -rn | head -n 35",
            "L2SYSDONE", timeout=30)
        print(f"===== /sys/kernel/slab top objects (ok={ok}) =====")
        print(text[:5000])
        g.send("poweroff")
    finally:
        g.close()
