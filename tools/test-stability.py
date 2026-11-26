#!/usr/bin/env python3
"""QEMU-only L2 stability tests. Python stdlib on host; existing guest tools only.

Quick run:   python3 tools/test-stability.py --iso build/out/l2.iso
Release:     python3 tools/test-stability.py --boots 100 --soak-hours 72
See tools/STABILITY.md for gates, scope, and result interpretation.
"""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shlex
import shutil
import signal
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
FAULT = re.compile(r"Kernel panic|Oops:|BUG:|general protection fault|"
                   r"Out of memory:|invoked oom-killer|rcl2:.*(?:panic|panicked)", re.I)


class Failure(RuntimeError):
    pass


class CommandFailure(Failure):
    """The guest shell completed a command with nonzero status."""


def meminfo(output):
    return {key: int(value) for key, value in
            re.findall(r"^(\w+):\s+(\d+) kB\s*$", output, re.M)}


def used_memory(info):
    if not {"MemTotal", "MemFree"} <= info.keys():
        raise Failure("incomplete /proc/meminfo")
    available = info.get("MemAvailable")
    if available is None:
        # Older kernels: estimate reclaimable memory, discount shared tmpfs.
        available = (info["MemFree"] + info.get("Buffers", 0) + info.get("Cached", 0)
                     + info.get("SReclaimable", 0) - info.get("Shmem", 0))
    return info["MemTotal"] - max(0, min(available, info["MemTotal"]))


class Guest:
    """One QEMU with a bounded receive buffer and full serial log on disk."""
    def __init__(self, args, directory, ram, disk=None):
        self.args = args
        self.directory = directory
        directory.mkdir(parents=True)
        self.log = (directory / "serial.log").open("wb", buffering=0)
        self.buffer = b""
        self.fault = None
        cmd = [args.qemu, "-m", str(ram), "-smp", str(args.cpus),
               "-accel", args.accel, "-display", "none", "-serial", "stdio",
               "-monitor", "none", "-nic", "none", "-no-reboot",
               "-cdrom", str(args.iso), "-boot", "d"]
        if disk:
            # disk is always a new regular file created by this harness.
            cmd += ["-drive", f"file={str(disk).replace(',', ',,')},format=raw,if=virtio,cache=directsync"]
        (directory / "command.json").write_text(json.dumps(cmd, indent=2) + "\n")
        try:
            self.process = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except BaseException:
            self.log.close()
            raise

    def read(self, timeout=0.2):
        if select.select([self.process.stdout], [], [], timeout)[0]:
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if chunk:
                self.log.write(chunk)
                self.buffer = (self.buffer + chunk)[-262144:]
                match = FAULT.search(self.buffer.decode(errors="replace"))
                if match:
                    self.fault = match.group(0)
                return True
        return False

    def healthy(self):
        if self.fault:
            raise Failure(f"guest fault: {self.fault}; see {self.directory / 'serial.log'}")
        if self.process.poll() is not None:
            raise Failure(f"unexpected QEMU exit {self.process.returncode}")

    def wait(self, token, timeout):
        deadline = time.monotonic() + timeout
        while token not in self.buffer:
            self.healthy()
            if time.monotonic() >= deadline:
                raise Failure(f"serial timeout waiting for {token!r}; see {self.directory / 'serial.log'}")
            self.read(min(0.2, max(0, deadline - time.monotonic())))
        self.healthy()

    def pause(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.healthy()
            self.read(min(0.2, max(0, deadline - time.monotonic())))

    def send(self, line):
        data = line.encode() + b"\n"
        if len(data) > 4000:
            raise Failure("serial command exceeds canonical TTY input limit")
        # Pace input through the emulated UART; large writes can lose bytes.
        for offset in range(0, len(data), 32):
            self.healthy()
            self.process.stdin.write(data[offset:offset + 32])
            self.process.stdin.flush()
            self.read(0.01)

    def command(self, script, timeout=None):
        self.buffer = b""
        marker = "L2CHECK_" + uuid.uuid4().hex
        # Split marker in the command so terminal echo cannot fake completion.
        wire = (f"sh -c {shlex.quote(script)}; r=$?; "
                f"printf '\\n%s%s:%s\\n' L2CHECK_ {marker[8:]} \"$r\"")
        self.send(wire)
        self.wait(marker.encode() + b":", timeout or self.args.command_timeout)
        # The status can arrive in a later serial read than the marker prefix.
        deadline = time.monotonic() + 2
        pattern = rb"\n" + marker.encode() + rb":(\d+)\r?\n"
        match = re.search(pattern, self.buffer)
        while not match and time.monotonic() < deadline:
            self.read()
            self.healthy()
            match = re.search(pattern, self.buffer)
        if not match:
            raise Failure("missing command exit status")
        output = self.buffer[:match.start()].decode(errors="replace").replace("\r", "")
        if int(match.group(1)):
            raise CommandFailure(f"guest command exited {match.group(1).decode()}: {output[-1200:]}")
        return output

    def boot(self):
        self.wait(b"L2#", self.args.boot_timeout)
        self.pause(0.5)
        # On the live ISO rcl2 is executed as /init, so its comm is "init".
        # Compare the executable itself rather than relying on its name.
        output = self.command("set -e; cmp -s /proc/1/exe /sbin/rcl2; id -u")
        if not re.search(r"^0$", output, re.M):
            raise Failure("requires the core L2 ISO with rcl2 PID 1 and a root serial shell")
        # ISO boot parameters are fixed by Limine. Enable debug behavior once
        # userspace is reachable, without rebuilding or changing the ISO.
        self.command("set -e; echo 1 > /proc/sys/kernel/panic_on_oops; echo 0 > /proc/sys/kernel/panic")
        self.pause(self.args.settle)

    def shutdown(self, reboot=False):
        self.buffer = b""
        self.send("kill -" + ("USR2" if reboot else "USR1") + " 1")
        deadline = time.monotonic() + self.args.shutdown_timeout
        while self.process.poll() is None and time.monotonic() < deadline:
            self.read()
            if self.fault:
                raise Failure("guest fault during shutdown: " + self.fault)
        while self.read(0):
            pass
        output = self.buffer.decode(errors="replace")
        expected = r"reboot: Restarting system" if reboot else r"reboot: Power down"
        if (self.process.poll() != 0 or not re.search(expected, output) or self.fault):
            raise Failure("clean shutdown/reboot not confirmed by kernel message and QEMU exit 0")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait()
        self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()


# Each payload runs in a separate shell. No fork bombs or unbounded allocators.
PROCESS_TEST = r'''
set -eu
i=0
while [ "$i" -lt CHURN ]; do ( : ) & wait "$!"; i=$((i+1)); done
# Separate shells prevent the launching shell from waiting for the orphan.
i=0
while [ "$i" -lt ORPHANS ]; do
    sh -c 'sleep 1 </dev/null >/dev/null 2>&1 & echo $!' >> /tmp/l2-orphans
    i=$((i+1))
done
sleep 3
while read -r p; do
    [ ! -e "/proc/$p/status" ] || { cat "/proc/$p/status"; exit 1; }
done < /tmp/l2-orphans
rm /tmp/l2-orphans
# A parent killed unexpectedly must also leave a child that PID 1 reaps.
sh -c 'sleep 2 </dev/null >/dev/null 2>&1 & echo $! >/tmp/l2-orphan; kill -KILL $$' &
p=$!; wait "$p" || :
sleep 4
read -r p </tmp/l2-orphan
[ ! -e "/proc/$p/status" ]
rm /tmp/l2-orphan
'''

ZOMBIES = r'''
set -eu
bad=0
for f in /proc/[0-9]*/status; do
    [ -r "$f" ] || continue
    while read -r k v rest; do
        if [ "$k" = State: ] && [ "$v" = Z ]; then echo "zombie: $f"; bad=1; fi
    done < "$f"
done
[ "$bad" = 0 ]
'''

SERVICE_TEST = r'''
set -eu
find_console() {
    # rcl2's console mode execs dash, so identify its terminal and parent,
    # rather than assuming the process still has rcl2 in its argv.
    for f in /proc/[0-9]*/fd/0; do
        [ -r "$f" ] || continue
        c=$(readlink "$f" 2>/dev/null) || continue
        if [ "$c" = /dev/tty1 ]; then
            p=${f#/proc/}; p=${p%/fd/0}
            if grep -q '^PPid:[[:space:]]*1$' "/proc/$p/status"; then
                echo "$p"; return 0
            fi
        fi
    done
    return 1
}
old=$(find_console)
[ "$old" -gt 1 ]
grep -q '^PPid:[[:space:]]*1$' "/proc/$old/status"
kill -KILL "$old"
i=0
while [ "$i" -lt 40 ]; do
    sleep 1
    new=$(find_console) || new=
    if [ -n "$new" ] && [ "$new" != "$old" ]; then
        grep -q '^PPid:[[:space:]]*1$' "/proc/$new/status"
        exit 0
    fi
    i=$((i+1))
done
exit 1
'''

CONFIG_TEST = r'''
set -eu
printf '%s\n' 'invalid directive' 'service incomplete' 'hostname Stability' \
    'service valid once /bin/true' > /tmp/l2-invalid.conf
/sbin/rcl2 --selftest /tmp/l2-invalid.conf > /tmp/l2-config-result 2>&1
cat /tmp/l2-config-result
grep -q 'config 1 service(s) accepted' /tmp/l2-config-result
grep -q 'hostname=Stability' /tmp/l2-config-result
grep -q 'selftest ok' /tmp/l2-config-result
rm /tmp/l2-invalid.conf /tmp/l2-config-result
'''

STRESS_TEST = r'''
set -eu
pids=
cleanup() { for p in $pids; do kill "$p" 2>/dev/null || :; done; wait || :; rm -f /tmp/l2-pressure; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
i=0
while [ "$i" -lt WORKERS ]; do (while :; do :; done) & pids="$pids $!"; i=$((i+1)); done
i=0
while [ "$i" -lt FANOUT ]; do sleep SECONDS & pids="$pids $!"; i=$((i+1)); done
# tmpfs pages are real guest RAM, released by cleanup. Cap at 1/8 MemAvailable
# and at --memory-stress-kib, retaining an explicit 4 MiB safety margin.
a=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
[ -n "$a" ] || a=$(awk '/^MemFree:/ {print $2}' /proc/meminfo)
n=$((a/8)); [ "$n" -le MEMORY ] || n=MEMORY
if [ "$((a-n))" -gt 4096 ] && [ "$n" -gt 0 ]; then
    dd if=/dev/zero of=/tmp/l2-pressure bs=1024 count="$n"
fi
sleep SECONDS
'''


def check_kernel(guest):
    output = guest.command("dmesg")
    match = FAULT.search(output)
    if match:
        raise Failure("kernel log reports: " + match.group(0))


def sample(guest, args, result, samples, phase):
    output = guest.command("echo MEMINFO; cat /proc/meminfo; echo PIDONE; cat /proc/1/status; "
                           "echo UPTIME; cat /proc/uptime")
    _, sep0, body = output.partition("MEMINFO\n")
    memory, sep, status = body.partition("PIDONE\n")
    if not sep0 or not sep:
        raise Failure("missing memory/PID 1 sample separator")
    info = meminfo(memory)
    rss = re.search(r"^VmRSS:\s+(\d+) kB", status, re.M)
    uptime = re.search(r"^UPTIME\n([\d.]+)", status, re.M)
    if not rss or not uptime:
        raise Failure("missing PID 1 RSS or guest uptime")
    row = {"time_utc": datetime.now(timezone.utc).isoformat(), "ram_mib": result["ram_mib"],
           "phase": phase, "uptime_seconds": float(uptime.group(1)),
           "used_kib": used_memory(info), "pid1_rss_kib": int(rss.group(1)),
           "available_estimated": "MemAvailable" not in info, **info}
    samples.append(row)
    with (args.output / "memory.jsonl").open("a") as stream:
        stream.write(json.dumps(row) + "\n")
    if phase == "idle" and row["used_kib"] >= args.budget_kib:
        result["errors"].append(f"idle RAM {row['used_kib']} KiB is not below {args.budget_kib} KiB")
    if phase == "idle":
        result["idle_used_kib"] = row["used_kib"]
        result["kernel_reserved_kib"] = result["ram_mib"] * 1024 - info["MemTotal"]
    return row


def exercise(guest, args, result, samples):
    for name, script in (
        ("process_churn_and_orphan_reaping", PROCESS_TEST.replace("CHURN", str(args.churn))
         .replace("ORPHANS", str(args.orphans))),
        ("console_service_restart", SERVICE_TEST),
        ("invalid_config_parser", CONFIG_TEST),
        ("bounded_cpu_memory_process_pressure", STRESS_TEST.replace("WORKERS", str(args.cpus))
         .replace("FANOUT", str(args.fanout)).replace("SECONDS", str(args.stress_seconds))
         .replace("MEMORY", str(args.memory_stress_kib))),
        ("no_remaining_zombies", ZOMBIES),
    ):
        print(f"    {name}", flush=True)
        try:
            guest.command(script)
            result["checks"].append(name)
        except CommandFailure as exc:
            result["errors"].append(f"{name}: {exc}")
            print(f"    FAIL: {name} (see serial log)", flush=True)
    guest.pause(args.settle)
    baseline = sample(guest, args, result, samples, "post_stress")
    deadline = time.monotonic() + args.soak_hours * 3600
    count = 0
    while time.monotonic() < deadline:
        guest.pause(min(args.sample_interval, deadline - time.monotonic()))
        guest.command("i=0; while [ \"$i\" -lt 10 ]; do ( : ) & wait \"$!\"; i=$((i+1)); done")
        row = sample(guest, args, result, samples, "soak")
        guest.command(ZOMBIES)
        check_kernel(guest)
        if row["used_kib"] - baseline["used_kib"] > args.max_growth_kib:
            raise Failure("available-memory usage grew beyond --max-growth-kib after stress")
        if row["pid1_rss_kib"] - baseline["pid1_rss_kib"] > args.max_pid1_growth_kib:
            raise Failure("PID 1 RSS grew beyond --max-pid1-growth-kib")
        count += 1
        print(f"    soak sample {count}: uptime {row['uptime_seconds']:.0f}s, "
              f"used {row['used_kib']} KiB, PID 1 {row['pid1_rss_kib']} KiB", flush=True)
    result["soak_hours_completed"] = args.soak_hours
    check_kernel(guest)


def matrix(args, report, samples):
    for ram in args.ram:
        result = {"ram_mib": ram, "required": ram >= args.min_ram, "boots_passed": 0,
                  "status": "INCOMPLETE", "shell_reached": False,
                  "boot_attempts": 0, "checks": [], "errors": []}
        report["matrix"].append(result)
        for attempt in range(1, args.boots + 1):
            print(f"RAM {ram} MiB: boot {attempt}/{args.boots}", flush=True)
            guest = Guest(args, args.output / f"ram-{ram}" / f"boot-{attempt:03}", ram)
            result["boot_attempts"] += 1
            try:
                guest.boot()
                result["shell_reached"] = True
                if attempt == 1:
                    sample(guest, args, result, samples, "idle")
                    exercise(guest, args, result, samples)
                else:
                    guest.command("test -r /proc/1/status")
                    check_kernel(guest)
                guest.shutdown(reboot=attempt % 2 == 0)
                result["boots_passed"] += 1
            except (Failure, OSError) as exc:
                result["errors"].append(str(exc))
                print(f"    FAIL: {exc}", flush=True)
                break
            finally:
                guest.close()
        result["status"] = ("PASS" if not result["errors"] else
                            "FAIL" if result["required"] else "EXPLORATORY_FAILURE")


MOUNT = "mkdir -p /mnt/l2-stability; mount -t ext4 /dev/vda /mnt/l2-stability"
VERIFY = ('cd /mnt/l2-stability; expected=$(cat committed.sha256); '
          'actual=$(sha256sum committed); [ "$actual" = "$expected" ]')


def fsck(args, disk, phase, repair=False):
    cmd = ["e2fsck", "-fy" if repair else "-fn", str(disk)]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=args.command_timeout)
    (args.output / f"fsck-{phase}.log").write_bytes(proc.stdout)
    if proc.returncode not in ((0, 1) if repair else (0,)):
        raise Failure(f"filesystem check failed ({proc.returncode}), see fsck-{phase}.log")
    return proc.returncode


def filesystem(args, report):
    result = {"status": "RUNNING", "errors": [], "checks": []}
    report["filesystem"] = result
    disk = args.output / "disposable-ext4.img"
    guest = None
    try:
        with disk.open("xb") as stream:
            stream.truncate(64 * 1024 * 1024)
        subprocess.run(["mkfs.ext4", "-q", "-F", "-O", "^orphan_file", str(disk)],
                       check=True, timeout=args.command_timeout)
        for phase in ("write", "clean-reboot", "recovery"):
            print(f"Filesystem: {phase}", flush=True)
            guest = Guest(args, args.output / f"fs-{phase}", args.fs_ram, disk)
            try:
                guest.boot()
                guest.command(MOUNT)
                if phase == "write":
                    # Nonzero, guest-generated pattern; overwrite and interrupted
                    # file are separate from this committed recovery reference.
                    guest.command("set -eu; cd /mnt/l2-stability; "
                                  "awk 'BEGIN {for(i=0;i<16384;i++) print i, i*17}' > committed; "
                                  "sha256sum committed > committed.sha256; sync; " + VERIFY)
                    digest = guest.command("cat /mnt/l2-stability/committed.sha256")
                    result["committed_sha256"] = re.search(r"[0-9a-f]{64}", digest).group(0)
                    guest.command("umount /mnt/l2-stability")
                    guest.shutdown(reboot=True)
                    fsck(args, disk, "clean-reboot")
                    result["checks"].append("committed_write_and_clean_reboot")
                else:
                    output = guest.command("set -eu; " + VERIFY + "; sha256sum committed")
                    if result["committed_sha256"] not in output:
                        raise Failure("committed data changed across reboot/recovery")
                    if phase == "clean-reboot":
                        result["checks"].append("hash_survived_clean_reboot")
                        # Signal readiness only after at least one write. Loop
                        # repeatedly overwrites a bounded 8 MiB scratch file.
                        guest.buffer = b""
                        guest.send("cd /mnt/l2-stability && dd if=/dev/zero of=inflight bs=4096 count=2048 && "
                                   "echo L2WRITE_''ACTIVE && while :; do "
                                   "dd if=/dev/zero of=inflight bs=4096 count=2048 2>/dev/null || break; done")
                        guest.wait(b"L2WRITE_ACTIVE\r\n", args.command_timeout)
                        guest.pause(0.2)
                        guest.process.kill()  # deliberate simulated power loss
                        guest.process.wait()
                    else:
                        guest.command("set -eu; cd /mnt/l2-stability; "
                                      "rm -f inflight; printf 'recovered\\n' > after-recovery; sync; "
                                      "grep -q recovered after-recovery; cd /; umount /mnt/l2-stability")
                        check_kernel(guest)
                        guest.shutdown()
                        fsck(args, disk, "after-recovery")
                        result["checks"].append("committed_hash_and_writes_after_crash_recovery")
            finally:
                guest.close()
                guest = None
            if phase == "clean-reboot":
                result["repair_exit_code"] = fsck(args, disk, "crash-repair", repair=True)
        result["status"] = "PASS"
    except (Failure, OSError, subprocess.SubprocessError, AttributeError) as exc:
        result["status"] = "FAIL"
        result["errors"].append(str(exc))
        print(f"    FAIL: {exc}", flush=True)
    finally:
        if guest:
            guest.close()


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return number


def nonnegative(value):
    number = float(value)
    if not 0 <= number < float("inf"):
        raise argparse.ArgumentTypeError("must be finite and nonnegative")
    return number


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", type=Path, default=ROOT / "build/out/l2.iso")
    ap.add_argument("--ram", default="16,32,64,256,1024", help="comma-separated MiB")
    ap.add_argument("--min-ram", type=positive, default=32, help="required support floor in MiB (default: 32)")
    ap.add_argument("--boots", type=positive, default=3, help="clean boots per RAM size (release: 100)")
    ap.add_argument("--soak-hours", type=nonnegative, default=0, help="soak per RAM size; default skips endurance")
    ap.add_argument("--sample-interval", type=positive, default=60, help="soak sampling interval in seconds")
    ap.add_argument("--budget-kib", type=positive, default=10240)
    ap.add_argument("--max-growth-kib", type=positive, default=1024)
    ap.add_argument("--max-pid1-growth-kib", type=positive, default=256)
    ap.add_argument("--churn", type=positive, default=1000)
    ap.add_argument("--orphans", type=positive, default=32)
    ap.add_argument("--fanout", type=positive, default=16)
    ap.add_argument("--memory-stress-kib", type=positive, default=4096)
    ap.add_argument("--stress-seconds", type=positive, default=10)
    ap.add_argument("--cpus", type=positive, default=2)
    ap.add_argument("--settle", type=nonnegative, default=10)
    ap.add_argument("--boot-timeout", type=positive, default=120)
    ap.add_argument("--command-timeout", type=positive, default=120)
    ap.add_argument("--shutdown-timeout", type=positive, default=45)
    ap.add_argument("--fs-ram", type=positive, default=256)
    ap.add_argument("--skip-filesystem", action="store_true", help="record filesystem category as SKIP")
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--accel", choices=("auto", "kvm", "tcg"), default="auto")
    ap.add_argument("--output", type=Path, help="new results directory (must not exist)")
    args = ap.parse_args(argv)
    try:
        args.ram = list(dict.fromkeys(positive(x.strip()) for x in args.ram.split(",")))
    except (ValueError, argparse.ArgumentTypeError):
        ap.error("--ram must contain positive integer sizes in MiB")
    if not any(ram >= args.min_ram for ram in args.ram):
        ap.error("--ram must include a size at or above --min-ram")
    if args.accel == "auto":
        args.accel = "kvm" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "tcg"
    args.iso = args.iso.resolve()
    if not args.iso.is_file():
        ap.error(f"ISO not found: {args.iso}")
    for tool in [args.qemu] + ([] if args.skip_filesystem else ["mkfs.ext4", "e2fsck"]):
        if not shutil.which(tool):
            ap.error(f"missing host tool: {tool}")
    if args.output is None:
        args.output = ROOT / "build/stability" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    args.output = args.output.resolve()
    # QEMU -drive uses a comma-separated option grammar. No user disks accepted.
    if "," in str(args.output):
        ap.error("--output cannot contain commas")
    if args.output.exists():
        ap.error("--output must be a new directory")
    return args


def write_report(args, report, samples):
    fields = sorted({key for row in samples for key in row})
    with (args.output / "memory.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(samples)
    required = [row for row in report["matrix"] if row["required"]]
    report["minimum_observed_boot_ram_mib"] = min(
        (row["ram_mib"] for row in report["matrix"] if row["shell_reached"]), default=None)
    report["passed"] = bool(required) and all(row["status"] == "PASS" for row in required)
    report["passed"] &= (not report["errors"] and report["iso_budget_passed"]
                         and report["filesystem"]["status"] in ("PASS", "SKIP"))
    report["release_gates_satisfied"] = bool(
        report["passed"] and args.boots >= 100 and args.soak_hours >= 72
        and report["filesystem"]["status"] == "PASS")
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


def main(argv=None):
    args = parse_args(argv)
    args.output.mkdir(parents=True)
    samples = []
    report = {"started_utc": datetime.now(timezone.utc).isoformat(),
              "configuration": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
              "iso_bytes": args.iso.stat().st_size, "matrix": [], "errors": [],
              "filesystem": {"status": "SKIP" if args.skip_filesystem else "NOT_RUN"},
              "limitations": ["BIOS core ISO only; UEFI needs its existing dedicated tests",
                              "bounded pressure, not deliberate OOM or process-limit exhaustion",
                              "invalid configuration tested via parser selftest, not a changed PID 1 boot config",
                              "console crash/respawn tested; failed service startup is not injected",
                              "QEMU process termination models guest power loss, not host storage power failure"]}
    report["iso_budget_passed"] = report["iso_bytes"] < 10 * 1024 * 1024
    with args.iso.open("rb") as stream:
        report["iso_sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
    print(f"Results: {args.output}\nISO: {report['iso_bytes']} bytes, "
          f"<10 MiB gate: {'PASS' if report['iso_budget_passed'] else 'FAIL'}", flush=True)
    try:
        matrix(args, report, samples)
        if not args.skip_filesystem:
            filesystem(args, report)
    except KeyboardInterrupt:
        report["errors"].append("interrupted; suite incomplete")
    except (Failure, OSError, subprocess.SubprocessError) as exc:
        report["errors"].append(str(exc))
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_report(args, report, samples)
    for row in report["matrix"]:
        print(f"{row['ram_mib']:4} MiB: {row.get('status', 'INCOMPLETE')} "
              f"({row['boots_passed']}/{args.boots} clean boots)")
    print(f"Filesystem: {report['filesystem']['status']}")
    print(f"Result: {'PASS' if report['passed'] else 'FAIL'}; "
          f"72h release gates: {'satisfied' if report['release_gates_satisfied'] else 'not satisfied'}")
    print(f"Report: {args.output / 'report.json'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    sys.exit(main())
