# Mission 21 — rcl2 PID1 lifecycle audit (DONE 2026-11-20)

Read-only audit of `rcl2/src/{start.s,main.c,mem.c,rcl.h,syscalls.h}` and
`rcl2/src/rs/rcl.rs`. **User WIP preserved**: the uncommitted memory
optimizations (Makefile opt-level=z / --no-rosegment -z norelro; main.c;
rcl.rs) were committed by the user as `cd0f88a` on branch
`campaigns/armageddon-ii` at 20:04 during this session. Nothing in rcl2 was
modified by the campaign. Any future rcl2 implementation must build on that
commit, not revert it.

## Architecture

- ~26.9 KiB static binary, no libc: C core (main.c, raw syscalls via
  syscalls.h) + Rust core (rcl.rs: config parser, bootflags, statm parse,
  banner) compiled `--crate-type staticlib -C opt-level=z -C panic=abort`,
  linked `-static --gc-sections -z max-page-size=4096 --no-rosegment
  -z norelro`. Entry `start.s:_start` → `rcl_enter(sp)`; if main ever
  returns, exit(1) + spin (PID1 must not exit).
- C/Rust ABI: fixed-size structs with compile-time size/offset asserts
  (Svc=507 B, Conf=8692 B; offsets pinned). Limits: 16 services, 6 args,
  8 env vars, name 24 / path 96 / arg 64 / env 64 / host 64 bytes.
  All state in static buffers (g_iobuf 2048, argv/env tables, Conf) —
  no heap allocator in PID1.
- Modes: `once`, `respawn`, `wait`. Config keys: hostname, klog, setenv,
  ctrlaltdel (reboot|poweroff), service. Bad lines skipped, not fatal.
  No config / parse failure → default respawn shell service (/bin/dash -i).

## Lifecycle (verified by code read + boot log + selftest)

1. `rcl_enter` dispatch: `--selftest` (config/cmdline/rss fixtures),
   `poweroff|reboot|halt` → `power_direct` (sync×2 + remount-ro + reboot
   syscall; works from any context, used by overlay/bin/shutdown wrappers),
   `console <dev>` → getty mode (tty1 service), else must be PID1.
2. `pid1_boot`: signals_setup (self-pipe O_NONBLOCK|O_CLOEXEC; handlers for
   HUP/INT/QUIT/TERM/USR1/USR2/PWR/CHLD/WINCH; SA_RESTORER, **no SA_RESTART**
   so blocking waits EINTR on shutdown; atomic flag bits — comment documents
   a stale-register bug fixed), mounts_setup, conf_load (Rust parser),
   klogctl level, hostname, env, bootflags from /proc/cmdline
   (`l2.rescue=1|single|s|emergency` → rescue_shell, never returns),
   argv_build, then per service: wait-mode → spawn + blocking wait;
   others → spawn (non-blocking); reap between spawns. rss_report
   (statm → KiB, budget 150), "boot complete", supervise().
3. `supervise`: ppoll on self-pipe, timeout = earliest respawn deadline
   (min 50 ms anti-busy-rearm; fallback 1 s poll if pipe creation failed).
   Each iteration: reap_children → take_actions → spawn_due → pipe_drain.
   Self-pipe closes the signal-before-ppoll lost-wakeup race (commented).
4. Reaping: wait4(-1, WNOHANG) loop; service pids mapped to state; orphan
   zombies reaped and logged ("rcl2: reap N"). -ECHILD ⇒ no children.
5. Respawn backoff: floor 1 s, ×2 per short-lived exit, ceiling 30 s;
   reset to floor after ≥30 s stable uptime (SB_STABLE_MS). Fork failure:
   3 s start, ×2, ceiling 30 s. Successful fork ≠ healthy: delay preserved
   across short runs (commented invariant).
6. Shutdown (`shutdown_run`, noreturn): g_closing=1 (suppresses respawn),
   SIGTERM to service pids then kill(-1,TERM), 5 s grace reaping,
   kill(-1,KILL), 3 s grace, sync×2 (100 ms apart), remount / ro best-effort,
   reboot(cmd). On reboot syscall failure: degrade to RB_HALT, then idle
   forever. Signal map: INT→ctrlaltdel action, USR2→reboot,
   TERM/USR1/PWR→poweroff, QUIT/HUP→halt.
7. Failure containment: Rust panic → `rcl_c_panic` writes to console with
   raw syscalls and idles PID1 forever (no exit). wait-mode EINTR path
   re-checks state after take_actions/reap sweep (child may be collected
   elsewhere). rescue_shell: dash -i on /dev/console with minimal env.
8. Observed boot (forensics log): booted → 3 services parsed → net wait
   (pid 51) → net exit 0 → tty1 (78) + ttyS0 (79) → resident 68 KiB
   (budget 150) → boot complete. Selftest: 48 KiB resident, supervision
   regression suite passes (`make -C rcl2 test`, build/test-supervision).

## Campaign C gap map (missions 22–30)

| mission | state | gap / note |
|---|---|---|
| 22 PID1 memory | 48–68 KiB ≪ 150 budget | user WIP (opt-level=z, segment merging) already targets this; measure before/after on cd0f88a, don't redo |
| 23 dependency resolution | partial | only `wait` ordering (net blocks boot). No `after=`/`needs=` graph. For 3 services wait suffices; a tiny `after=<svc>` keyword is the minimal generalization |
| 24 parallel startup | mostly exists | non-wait services already spawn without blocking; wait services serialize by design (rcl2.conf: net first so VM tuning precedes shell allocation — RAM-measurement stability). Document, don't "fix" |
| 25 crash recovery | exists | respawn+backoff, panic-idle, rescue mode. Needs fault-injection proof (30) |
| 26 watchdog supervision | gap | no /dev/watchdog handling, no hung-service detection. CONFIG_WATCHDOG not in fragment. Minimal: optional `watchdog` config + softdog module; decide scope |
| 27 health monitoring | gap | backoff tracks uptime only; no check command / state export. Minimal: `rcl2 status` reading a /run state file, or `health=` cmd |
| 28 resource controls | gap, constrained | **CGROUPS=n in kernel** ⇒ cgroups impossible; use RLIMIT_* in child post-fork (rlimit config key). Fits tiny philosophy |
| 29 emergency recovery | exists | l2.rescue=1/single/s/emergency → rescue shell; verify under 30 |
| 30 lifecycle/fault tests | partial | build/test-supervision + tools/test-stability.py exist; extend with shutdown-grace, crash-loop, fork-bomb (RLIMIT_NPROC), rescue-boot, OOM cases |

## Risks / notes for implementers

- rcl2 is x86_64-only raw-syscall code; the C/Rust struct ABI is asserted at
  compile time — any config-key addition must update BOTH sides + asserts.
- No SA_RESTART is deliberate; new blocking waits must handle EINTR and
  re-check shutdown flags (see svc_wait_boot pattern).
- Signal handlers must stay async-safe (flag bit + nonblocking pipe write).
- PID1 must never exit: every new error path ends in idle-loop or rescue,
  not return/exit.
- The user's WIP is now committed (cd0f88a); build on it. Keep changes
  small and reviewable; `make -C rcl2 test` must stay green.

## Addendum (20:25) — WIP committed and measured

- `cd0f88a` (20:04) commits the WIP: Makefile `-C opt-level=z`,
  `--no-rosegment -z norelro`; main.c/rcl.rs memory optimizations.
- Peer rebuild (20:06) picked it up: **binary 26,904 → 17,184 B (−9,720 B,
  −36%)**; initramfs 426,833 → 421,275 B; core ISO hash changed accordingly
  (kernel itself byte-identical, 0fa6cfc8… — reproducibility holds).
- `build/out/rcl2 --selftest`: **resident 48 KiB (budget 150), selftest ok**
  — resident unchanged vs pre-WIP (static Conf/buffers dominate RSS); the
  win is storage/initramfs size. Boot-resident (was 68 KiB on the old
  binary) re-measure pending from the next boot log.
- Mission 22 credit: this improvement is the USER's WIP, verified here —
  not campaign-authored. Any further PID1 memory work must build on cd0f88a.