#!/usr/bin/env python3
"""Deterministic signal test for rcl2 PID 1 under qemu.

Serial-stdio input is lossy for the trailing LF (seen in earlier runs:
every character echoed, no line terminator, dash never processed the
line). Mitigation: send every command terminated with BOTH '\\r' and
'\\n' (ICRNL makes them equivalent to the guest) .

Phase A: SIGUSR2 -> 'shutting down: reboot request', '-no-reboot' exit.
Phase B (fallback if A is silent): SIGUSR1 -> 'shutting down: poweroff
          request' + kernel 'reboot: Power down' + qemu exit 0.
Prints the complete serial capture plus raw repr of the tails."""
import os, re, select, subprocess, sys, time, fcntl

root = "/home/maiake/L2"
boot = "/tmp/opencode/rcl2boot"
cmd = ["qemu-system-x86_64", "-accel", "tcg", "-m", "64", "-display", "none",
       "-serial", "stdio", "-monitor", "none", "-no-reboot", "-nic", "none",
       "-kernel", f"{root}/root/boot/vmlinuz-7.2.11",
       "-initrd", f"{boot}/rootfs.cpio.gz",
       "-append", "rdinit=/init console=ttyS0,115200 loglevel=4 panic=-1"]

q = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT)
fd = q.stdout.fileno()
fcntl.fcntl(fd, fcntl.F_SETFL, os.O_NONBLOCK)

def send(line: str):
    q.stdin.write((line + "\r").encode())
    q.stdin.flush()
    time.sleep(0.2)                       # interleave in 16550 RX FIFO
    q.stdin.write((line + "\n").encode())
    q.stdin.flush()

buf = b""
def read_all():
    global buf
    r, _, _ = select.select([fd], [], [], 0.05)
    if r:
        try:
            buf += os.read(fd, 65536)
        except OSError:
            pass

# phase 0: boot to prompt
boot_end = time.time() + 150
while time.time() < boot_end:
    read_all()
    if b"L2# " in buf:
        break
time.sleep(0.7)
read_all()

send("kill -USR2 1")                      # SIGUSR2 = 12 -> reboot request
t_a = time.time() + 12
while time.time() < t_a:
    read_all()
    if b"reboot request" in buf or q.poll() is not None:
        break

usr2_ok = b"reboot request" in buf
if not usr2_ok and q.poll() is None:      # fallback: SIGUSR1 = 10 poweroff
    send("kill -USR1 1")
    t_b = time.time() + 25
    while time.time() < t_b:
        read_all()
        if (
            b"poweroff request" in buf or
            b"Power down" in buf or
            q.poll() is not None
        ):
            break

end_wait = time.time() + 10
while q.poll() is None and time.time() < end_wait:
    read_all()

exit_code = q.poll()
try: q.kill()
except Exception: pass

text = buf.decode("utf-8", "replace")
print("== full serial capture ==")
print(text)
print("== raw tail repr ==")
print(repr(buf[-400:]))
print("== verdict ==")
print("SIGUSR2 shutdown line:", "shutting down: reboot request" in text)
print("SIGUSR1 shutdown line:", "shutting down: poweroff request" in text)
print("kernel reboot line:", bool(re.search(r"reboot: (Restarting|Power)", text)))
print("qemu exit code:", exit_code)
m = re.search(r"rcl2: resident (\d+) KiB", text)
print("resident RAM:", m.group(1) if m else "?", "KiB (budget 150)")
passed = (
    ("shutting down: reboot request" in text or
     "shutting down: poweroff request" in text)
    and re.search(r"reboot: (Restarting|Power)", text)
    and exit_code == 0
    and m and int(m.group(1)) < 150
)
print("RESULT:", "PASS" if passed else "FAIL")
sys.exit(0 if passed else 1)
