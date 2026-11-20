#!/usr/bin/env python3
"""Verify CLEAN ACPI poweroff: boot the lowram ISO, send `poweroff`, and check
QEMU exits ON ITS OWN (no kill).  With SUSPEND=n this proves the X86
ACPI_SYSTEM_POWER_STATES_SUPPORT path still registers acpi_power_off (S5).

Usage: poweroff_check.py [ISO] [MiB]   (default out/l2-lowram.iso, 16)
Log: campaigns/poweroff-<MiB>M-bios.log.  Exit 0 only on a clean self-exit.
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
logpath = HERE / "campaigns" / f"poweroff-{size}M-bios.log"
logpath.parent.mkdir(parents=True, exist_ok=True)
cmd = m.build_cmd(size, "bios", 2, iso=iso)
print(f"poweroff check: {size}M BIOS ISO {iso}", flush=True)
with open(logpath, "ab") as fh:
    g = m.Guest(cmd, fh)
    exited = False
    try:
        if not g.read_until(m.PROMPT, 180):
            print("FAIL: no L2# prompt before timeout", flush=True)
            sys.exit(1)
        time.sleep(5)
        g.send("poweroff")
        end = time.monotonic() + 25
        while time.monotonic() < end:
            g._drain(0.5)
            if g.p.poll() is not None:
                exited = True
                break
        rc = g.p.poll()
        text = bytes(g.all).decode(errors="replace")
        tail = "\n".join(text.splitlines()[-12:])
        print(f"qemu_exit={rc} clean_self_exit={exited}", flush=True)
        print("tail:\n" + tail, flush=True)
        sys.exit(0 if exited else 1)
    finally:
        g.close()   # only kills if QEMU is still running (i.e. poweroff failed)
