# L2 stability testing

Run the harness on the host. It starts QEMU itself and runs stress commands only
through the guest serial console. No packages or test agents are added to L2.
Host dependencies are Python **3.11+**, `qemu-system-x86_64`, and e2fsprogs
(`mkfs.ext4`, `e2fsck`). It needs neither root nor network access. KVM is used
when accessible, otherwise TCG; `--accel tcg` forces emulation.

```sh
# Short screening run: three clean boots at each size; no endurance soak.
python3 tools/test-stability.py --iso build/out/l2.iso

# Initial 24-hour soak at one supported size.
python3 tools/test-stability.py --ram 256 --min-ram 256 --boots 100 --soak-hours 24

# Full matrix release run. Soak duration is PER size, run sequentially.
python3 tools/test-stability.py --boots 100 --soak-hours 72

# Faster development check, including persistent-disk recovery.
python3 tools/test-stability.py --ram 256 --min-ram 256 --boots 2 \
  --churn 30 --orphans 4 --stress-seconds 2 --settle 2 \
  --soak-hours 0.003 --sample-interval 5

# Host-side harness regression checks; no VM required.
python3 -m unittest discover -s tools -p 'test_stability_harness.py'
```

The default matrix is **16, 32, 64, 256, 1024 MiB**. `--min-ram 32` declares the
required support floor for this run; this is a testing assumption, not a claim
that the image supports 32 MiB. Choose the floor explicitly for a release.
Sizes below it are exploratory: failures are recorded but do not fail the run.
Supported configurations must pass every configured boot and check. A lower
observed boot minimum does not change the declared support floor.

Use the **core ISO**, whose root serial console shows `L2#`. Installed
system boot and UEFI need the existing installer and `tools/ramtest.py` tests.
This harness verifies PID 1's executable matches `/sbin/rcl2`, even when its
process name is `init`.

## Coverage and gates

| Category | Automated check |
| --- | --- |
| Image size | ISO strictly smaller than 10 MiB |
| Boot reliability | Default 3, configurable 100+ cold VM boots per size; kernel-confirmed clean poweroff/reboot alternating between attempts |
| Idle memory | After settling, `MemTotal - MemAvailable` strictly below 10,240 KiB; fallback reclaimable-memory estimate identified in samples |
| PID 1 | 1,000 child exits, 32 orphans, a parent killed with SIGKILL, no remaining orphan PIDs or zombies |
| Service recovery | Kill the supervised tty1 console, require a replacement child of PID 1 within 40 seconds |
| Invalid config | Separate `rcl2 --selftest` parser fixture with malformed directives and a valid service |
| Stress | Two CPU workers by default, 16 simultaneous sleeping children, bounded tmpfs allocation, followed by zombie and kernel-log checks |
| Endurance | Optional 24–72 hours per size, command liveness, short process churn, memory/PID 1 RSS sampling, zombie and kernel-log checks |
| Filesystem | Separate 64 MiB disposable ext4 disk at 256 MiB RAM; hashes across clean reboot and simulated power loss; offline fsck and post-recovery writes |

Pressure is finite. The allocation is capped at 4 MiB and one eighth of current
available guest RAM, with a 4 MiB margin. Controls include `--churn`, `--orphans`,
`--fanout`, `--cpus`, `--memory-stress-kib`, and `--stress-seconds`. Guest command,
boot, and shutdown timeouts terminate stuck VMs. Ctrl-C or SIGTERM saves an
incomplete, failing report and stops the current QEMU.

Soak growth is compared with a **post-stress** sample, allowing warm-up and
cache effects. Defaults permit at most 1,024 KiB growth in used RAM and 256 KiB
growth in PID 1 RSS. Tune `--max-growth-kib` and `--max-pid1-growth-kib` for a
release's agreed thresholds. These are regression gates, not proof that a
system has no leaks. Raw `MemFree`, `MemAvailable`, `Cached`, `Slab`,
`SReclaimable`, `PageTables`, and other available meminfo fields are retained.
Kernel-reserved RAM (`configured RAM - MemTotal`) is reported separately from
idle usage. Filesystem cache alone is not classified as a leak.

The filesystem test creates its own regular image file. It accepts **no host
block device**. Filesystem tools run only after QEMU has exited. A patterned,
synced reference and its host-recorded SHA256 must survive. Interrupted writes
target a separate bounded scratch file; its incomplete contents are not
required to survive. Offline `e2fsck -fy` may recover the journal/repair the
filesystem (exit 0 or 1), followed by a guest mount, reference verification,
new writes, clean unmount/poweroff, and a read-only `e2fsck -fn` (exit 0).
Repair output is preserved for review. This models guest power loss and does
not emulate a host disk losing power or guarantee arbitrary interrupted data.

The harness enables `panic_on_oops=1` and `panic=0` after the serial shell is
available. To apply the behavior during boot too, add `oops=panic panic=0` to
`config/cmdline` before building your debug ISO. ISO boots use Limine's embedded
command line; QEMU's `-append` is for direct kernel boot. See the
[kernel parameter documentation](https://kernel.org/doc/html/latest/admin-guide/kernel-parameters.html)
and [QEMU invocation reference](https://qemu-project.gitlab.io/qemu/system/invocation.html).

## Results

Each invocation creates a new directory under `build/stability/`. Use
`--output PATH` to select a different **new** directory. Existing directories
are refused, so logs and disk evidence cannot be overwritten.

- `report.json`: ISO hash/size, configuration, results by RAM size, errors,
  minimum observed boot RAM, filesystem checks, limitations and release gate.
- `memory.jsonl`, `memory.csv`: timestamped memory samples and guest uptime.
- `ram-*/boot-*/serial.log`, `fs-*/serial.log`: full VM serial logs.
- `command.json` alongside each serial log: exact QEMU invocation.
- `disposable-ext4.img`, `fsck-*.log`: persistent disk and offline recovery logs.

Exit **0** means the configured checks passed; exit **1** means a gate failed or
execution was interrupted; invalid arguments/dependencies exit **2**. An ISO
over the size gate still runs all VM checks, then fails the overall result.
`--skip-filesystem` explicitly records a skip and cannot satisfy release gates.
A short screening pass also cannot satisfy release gates. The
`release_gates_satisfied` field requires all specified required configurations
to pass, at least 100 clean boots each, at least 72 hours soak each, ISO under
10 MiB, and filesystem recovery passing.

This suite does not inject failed service startup, replace PID 1's boot config,
deliberately invoke the OOM killer, exhaust process limits, or exercise kernel
fault-injection interfaces. Those remain separate VM scenarios. Review the
recorded limitations and repair logs before treating a passing automated run
as release approval.

## Low-RAM release screening

From the repository root, run the following command with the published digest or the independently recorded digest for the candidate under review.

```sh
python3 tools/verify-lowram.py path/to/l2-lowram.iso EXPECTED_SHA256
```

The command checks the digest and strict 10 MiB size limit on a private snapshot, runs `make check`, then boots that snapshot under BIOS at 10 MiB and UEFI at 128 MiB. Both runs use two virtual CPUs, a 40-second settle, a 50 KiB PID 1 RSS ceiling, a fresh shell response and a successful poweroff. Missing QEMU or OVMF fails screening. A matching hash establishes artifact identity. Publisher authenticity requires a trusted source for that hash.

The v3 profile targets a trusted serial VM. Its root console is unauthenticated, and its kernel omits IP networking, ext4, SMP and several CPU mitigations. The hardware media marker and restricted mount flags still require a trusted boot medium. Signed package distribution, authenticated hardware-pack pins, bare-metal qualification and the endurance gates have separate acceptance criteria.
