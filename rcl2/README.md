# rcl2 - a tiny Linux init for L2

`rcl2` is L2's init: it boots userspace as **PID 1** and stays as the sole
resident supervisor. One binary, two cores:

- **C core** (`src/main.c`, `src/start.s`, `src/mem.c`, `src/syscalls.h`):
  raw syscalls via `syscall(2)` only - fork/exec, wait/reap, signals,
  mounts, console, `reboot(2)`. **No libc is linked at all** (freestanding
  `-nostdlib`), so there is no glibc, no musl, no GNU runtime.
- **Rust core** (`src/rs/rcl.rs`): `#![no_std]`, zero crates, compiled as a
  static library and linked straight into the binary. It owns *parsing*:
  `/etc/rcl2.conf`, kernel cmdline flags and `/proc/self/statm`, writing
  into a pinned `#[repr(C)]` ABI asserted identical on both sides
  (`size_of` **and** `offset_of` checked twice, in C and in Rust).

It is a drop-in for L2's `script/init/init.c`: same boot mounts, same
console wording/PS1 (`L2# `), same `l2.rescue=1` rescue shell, and it
accepts the direct `poweroff` / `reboot` argv that `l2(8)` execs
(`assemble-root.sh`: `exec /init "$1"`).

## Non-GNU toolchain (build note)

Everything on this build path is non-GNU by design:

| role    | tool      | origin      |
|---------|-----------|-------------|
| C       | `clang`   | LLVM        |
| Rust    | `rustc`   | rust + LLVM |
| linker  | `ld.lld`  | LLVM        |
| strip/size/ar | `llvm-*` | LLVM   |

No gcc, no GNU binutils, no musl-gcc, no glibc headers - and since the binary
never links libc, the musl toolchain used elsewhere in L2 isn't needed here
either. The final link is `-nostdlib`: only our objects, rust core, and a
handful of mem stubs (`memcpy`/`memcmp`/`bcmp`...) that freestanding
compilers are allowed to emit.

## RAM budget: under 150 KiB

```
$ make && ls -l build/rcl2
-rwxrwxr-x build/rcl2                     25 640 bytes, stripped static ELF
$ ./build/rcl2 --selftest
rcl2: config 5 service(s) accepted (bad lines skipped)
rcl2: resident 48 KiB (budget 150)
$ make size
   text     data      bss      dec
  24234       128    25856    50218   build/rcl2
```

- static image: **25.6 KB** on disk (24.2 KB of it code).
- resident at runtime: **well under 150 KiB** - `--selftest` reports the
  process's own resident pages by parsing `/proc/self/statm` (48-60 KiB
  while exercising the whole config machinery); booted for real as PID 1
  under qemu (L2 kernel, initramfs), the supervisor reports **60-64 KiB**
  on every boot, and keeps running with a respawned shell behind it
  (see `verification/reboot-usr2.log`).
- why it stays small: no heap anywhere, fixed-size static buffers only
  (config 8.7 KB, service tables, one 512 B log line), no unwinding
  (`-C panic=abort`), `--gc-sections`, single 4 KiB page alignment.

On a live system, verify with `cat /proc/1/status` (`VmRSS`) or the boot log
line rcl2 prints after services start.

## Stability design

- **PID 1 never exits.** Every failure (mount, fork, exec, parse) is logged
  and either retried with backoff or degraded (e.g. missing config -> default
  respawn `/bin/dash -i`, `console` open failure -> inherited fds).
- **Single thread, no locks.** All decisions happen between `ppoll()`
  returns; signal handlers only `write()` to a self-pipe and set an atomic
  bit.
- **Zombie-proof.** Every child is reaped (`wait4(-1, WNOHANG)` each loop),
  unknown children included.
- **Backoff-bounded restarts** for respawn services: 1s, 2s, 4s, ... capped
  at 30s; resets to 1s after a service has been up for 30s.
- **Bounded parsing.** The Rust parser never allocates, never unwraps, never
  panics on trash input: bad lines are skipped, good lines still load, and a
  completely bad config still boots (default shell service). The `#[panic_handler]`
  logs to console and idles PID 1 instead of dying.
- **Clean child env/signal state**: children start with an empty signal
  mask, `setsid()`, `O_CLOEXEC`-guarded pipes, PATH/HOME/USER/TERM env.
- **No duplicate console fallback**: an explicit `/dev/ttyX` that cannot be
  opened fails (supervision backs off) instead of silently reusing
  `/dev/console`, so two console services cannot spawn two shells on the same
  terminal. Console services must use `respawn`, not `wait`.
- **Fixed ABI** between C and Rust with size+offset static asserts in both
  languages so neither side can drift silently.

## Boot sequence (mirrors L2 stage 1)

1. `signals_setup()` (self-pipe + SA_RESTORER via asm trampoline)
2. mount `devtmpfs /dev` (`mode=0755`), reopen `/dev/console` on fd 0/1/2
3. mount `proc /proc` (`/dev/fd|stdin|stdout|stderr` symlinks), `sysfs /sys`
4. `/dev/pts` (`mode=0620,ptmxmode=0666`), `/dev/shm` (`mode=1777`),
   and on systems with `/etc/l2/installed`: `/run` (`mode=0755`), `/tmp` (`1777`)
5. read `/proc/cmdline` (Rust: `l2.rescue=1` / `single` / `s` / `emergency` / `-b`)
6. parse `/etc/rcl2.conf` (Rust), set hostname (default `L2`), klogctl
   console level (optional `klog N`), build env (PATH, HOME, USER, TERM, PS1
   `L2# ` plus `setenv` entries)
7. rescue flag -> L2 recovery shell loop (`setsid`+`TIOCSCTTY`, banner, respawn 1s)
8. spawn services in file order: `wait` mode blocks until exit (rc-script
   style), `once`/`respawn` continue
9. print resident RAM, enter the supervise loop (`ppoll` + self-pipe)

## Shutdown

| signal           | action                                   |
|------------------|------------------------------------------|
| SIGINT (ctrl-alt-del) | reboot (or poweroff if `ctrlaltdel poweroff`) |
| SIGUSR2          | reboot                                   |
| SIGTERM, SIGUSR1, SIGPWR | poweroff                          |
| SIGQUIT, SIGHUP  | halt                                     |

Sequence: TERM to services + `kill(-1, TERM)`, 5 s grace while reaping;
`kill(-1, KILL)`, 3 s drain; `sync()` twice; remount `/` read-only
(best-effort); `reboot(2)`; on failure degrade to halt and idle. PID 1
itself can never be killed by stray signals: anything it does not handle is
ignored by the kernel.

## rcl2.conf

```
hostname L2
klog 7                       # optional console loglevel, 0 = kernel default
setenv KEY=VALUE             # up to 8 extra service env vars
ctrlaltdel poweroff          # or reboot (default)

service NAME (once|respawn|wait) PATH [ARGS...]   # up to 6 args
```

- `respawn` - supervised forever, restarted with 1s..30s backoff
- `once` - start it, log its exit, move on
- `wait` - run it at boot and block until it exits (rc script style)

Comments (#), CRLF/tabs, blank lines, and unknown directives are skipped;
bad but earlier lines do not cancel later services. See `sample.conf` for
L2's equivalent of the s6 boot set (`rcl2 console` gettys, `mdev -df`,
`ip link set lo up`).

## One binary, four modes

```
rcl2                       # PID 1 boot (kernel: rdinit=/init or init=/sbin/rcl2)
rcl2 console [/dev/ttyX]   # getty-style login shell (dash -i), spawnable as a service
rcl2 poweroff|reboot|halt  # direct; this is what l2(8) execs
rcl2 --selftest [file]     # parse self check + RSS report, exits 0, safe anywhere
```

## Build & install

```
make          # build/rcl2
make selftest # parse + memory self check (safe as non-root)
make test     # selftest + deterministic supervisor regression tests
make size     # section sizes + archive listing
```

The supervisor tests use a fake clock and syscall responses to check repeated
crashes, the 30-second backoff cap, recovery after a stable run, fork failures,
and suppression of restarts for once services and during shutdown. They run
without root, real service processes, or a VM, using the same freestanding
toolchain as the init binary.

Drop it into the L2 image the same place `init.c` lands
(`assemble-root.sh`: `$cc -Os -static script/init/init.c -o "$dest/init"`):
build rcl2 and install as `/init` (or keep `/init` and point kernel cmdline
at `init=/sbin/rcl2`), then stage console services via `/etc/rcl2.conf`.
`l2 poweroff` / `l2 reboot` keep working unchanged: they exec this binary
with the matching argv. `measure-ram.py` keeps working: the `L2# ` prompt and
`l2 poweroff` path are unchanged, and `rdinit=/init l2.rescue=1` boots into
the same recovery shell.

## Layout

```
rcl2/
  Makefile           clang + rustc + ld.lld, non-GNU, no libc
  sample.conf        L2-style /etc/rcl2.conf
  GLM-HANDOFF.md     verification status (live-boot evidence, bug log)
  verification/      qemu boot-test driver + passing serial capture
  src/syscalls.h     x86_64 syscall table + constants
  src/rcl.h          C<->Rust ABI struct + static asserts
  src/start.s        _start + rt_sigreturn trampoline
  src/main.c         PID 1 core: boot, supervise, console, power, selftest
  src/mem.c          freestanding memcpy/memset/memcmp/strlen stubs
  src/rs/rcl.rs      Rust core: config/cmdline/statm parser, panics, banner
```

## Verified live (qemu, real PID 1)

Booted `/init` = rcl2 with L2's own `vmlinuz-7.2.11` and a minimal
initramfs (`bin/dash`, `/etc/rcl2.conf`), serial console driven by
`verification/test-boot.py`:

```
rcl2: config 1 service(s) parsed
rcl2: start shell pid 33
rcl2: resident 60 KiB (budget 150)
rcl2: boot complete: 1 service(s), signals armed
L2# kill -9 $$          -> rcl2: shell exited (signal 9), retry in 1s
L2#                     <- respawned shell (backoff verified)
L2# kill -USR2 1        -> rcl2: shutting down: reboot request
reboot: Restarting system  (qemu exits 0)
```

Every signal path to PID 1 (SIGUSR2 reboot, SIGUSR1 poweroff - the latter
also verified independently by a second reviewer) and the respawn/backoff
loop have live boots behind them. Note for anyone driving the serial
console from scripts: qemu's stdio chardev occasionally swallows the
trailing newline of a pasted line; terminate commands with both `\r` and
`\n` as `test-boot.py` does.

## Limits (deliberate)

- x86_64 only (syscall numbers, assembly entry/restorer).
- No utmp/wtmp, no cgroups, no serial line discipline beyond the console
  defaults L2 uses, no per-service env vars (global `setenv` only).
- Services that want a session leader should set it up themselves
  (`rcl2 console` does), or be `getty`-style binaries.
- 16 services / 6 args / 96 B paths - deliberately small, changable in
  `src/rcl.h` + `src/rs/rcl.rs` (both sides must assert the same layout).
