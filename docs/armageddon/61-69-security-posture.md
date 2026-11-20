# Missions 61/62/63/66/69 — security posture audit (2026-11-20, partial)

Evidence: canonical snapshot `build/campaigns/baseline-artifacts/kernel.config`
(stable while mission-05 rebuilds run), code reads cited inline, peer finding
from L2-ea independently VERIFIED by lead. No source changes made; fixes are
routed to the owning lanes. Threat model assumption: L2 is a single-user
root tool distro; physical access ≈ root by design; network exposure minimal
at idle (no listeners; DHCP client only; wifi on demand).

## F1 — l2net accepts module packs from ANY medium (VERIFIED, HIGH)

- `overlay/bin/l2net:find_pack` mounts any iso9660 block device in loop order
  (`L2_PACK_DEV`, sr0, sr1, sd*, vd*, nvme*, mmcblk*) and calls `use_pack` if
  `/l2/wifi` or `/l2/hw` exists — **no L2-LIVE marker requirement**. The
  marker check only stops retries on a genuine L2 medium WITHOUT a pack.
- Contrast: `l2install:find_medium` REQUIRES `$MEDIUM/l2/L2-LIVE`
  (l2install:124,131) — asymmetric trust (L2-ea finding, confirmed).
- Impact: `use_pack` symlinks `/lib/modules/$KREL` to the medium's modules
  and sets `firmware_class.path`; `load_modules` then `modprobe -a` by
  modalias ⇒ **ring-0 code load from untrusted media** (CONFIG_MODULE_SIG=n,
  no enforcement) + attacker-controlled firmware blobs. Live system runs as
  root with CAP_SYS_MODULE.
- Exploit scenarios: (a) live boot from USB (sd*) with attacker CD in sr0 —
  sr0 is tried FIRST, attacker pack wins over the genuine boot medium;
  (b) RAM-only rescue / direct-kernel boot with no genuine medium attached;
  (c) `l2net modules` / `wifi-start` re-runs on demand. Installed systems
  short-circuit on `/lib/modules/$KREL` (root fs) — not affected. A genuine
  boot medium WITH a pack found first also wins (usual live-ISO case).
- Fix lane + AGREED PHASING (L2-ea W6, 21:30): phase 1 (their lane,
  queued) = l2net L2-LIVE marker requirement + fail-closed pack search +
  l2install F3/F4/F5/F7/F14 + N3 set -f (their finding IDs, see
  docs/armageddon-ii-progress.md §W6). Phase 2 = pack integrity manifest
  with the manifest hash PINNED IN THE INITRAMFS (boot-chain-trusted
  anchor; marker alone is forgeable text) verified by l2net before use —
  needs l2 build-script edits (OUR deferred lane; coordinate after mission
  05 lands). REFINED DESIGN (L2-ea accepted caveat, 21:50): pin lives in
  TWO places — initramfs /etc/l2/pack-hash (ISO build, live/rescue) AND
  ext4 root /etc/l2/pack-hash (l2install, installed). Extra motivation:
  the installed /lib/modules/$KREL short-circuit is the ONLY current guard;
  if that dir goes missing (kernel upgrade w/o modules, corruption, partial
  install) find_pack falls through to medium search and F1 resurrects on
  installed systems — root-side pin closes the fall-through. Format
  contract (per-pack manifests, pin = manifest hashes, verification chain,
  fail-closed) drafted for co-sign: docs/armageddon/p2-pack-pin-contract.md.
  MODULE_SIG stays kernel-lane + user decision (size/key trade-off, §61).
  Mission-70 regression test: ours, after W6 commit hashes.
- Interim mitigation: boot/live-run with no untrusted media attached;
  regression test for this finding belongs in mission 70 once W6 lands.

## 61 — kernel attack surface (audited; decisions pending)

Off (deliberate tiny-distro trade-offs): CPU_MITIGATIONS (⇒ Spectre V1/V2
"Vulnerable" at boot, no retpoline/PTI/BHI/etc.), SECCOMP, STACKPROTECTOR,
MODULE_SIG, SECURITY (no LSMs), KEYS, AUDIT, FORTIFY_SOURCE,
HARDENED_USERCOPY, X86_MCE, CGROUPS, IPV6, NETFILTER, KEXEC, CRASH_DUMP,
BPF_SYSCALL, KPROBES, FTRACE, DEBUG_FS, IO_URING, USERFAULTFD, COMPAT/
IA32_EMULATION, MODIFY_LDT_SYSCALL, X86_MSR/CPUID, DEVMEM, PROC_KCORE,
PROC_PAGE_MONITOR, HUGETLBFS, IKCONFIG, COREDUMP, BINFMT_MISC.
On (hardening present): KASLR (RANDOMIZE_BASE+RELOCATABLE),
STRICT_KERNEL_RWX+STRICT_MODULE_RWX, X86_KERNEL_IBT, INIT_STACK_ALL_ZERO,
VMAP_STACK, MAGIC_SYSRQ (physical-access debug; document).
Absent symbols in 7.3.3 (verify, don't assume): X86_SMAP/X86_SMEP/
PAGE_TABLE_ISOLATION/MITIGATION_* — no =y or "is not set" lines found;
likely removed/unconditional or folded under CPU_MITIGATIONS. Follow-up:
check /proc/cpuinfo flags + dmesg on a real boot (mission 82 matrix logs).
X86_VSYSCALL_EMULATION=y — legacy attack surface, droppable (mission 05
candidate already listed).
Recommendations (NO config edits now — mission 05 owns kernel.fragment
concurrently; decisions → campaign J / user): document posture in README;
evaluate STACKPROTECTOR_STRONG (~1–3% size), SECCOMP (~30 KiB, enables
l2pm/l2net sandboxing), MITIGATION_RETPOLINE/SPECTRE_V2 (perf cost),
MODULE_SIG (closes F1 class; key management + size). All trade against the
10 MiB ISO / 15 MiB idle budgets — measure before enabling.

## 62 — PID1 privilege handling (audited)

- rcl2 runs as root, no capability separation (no libcap; raw syscalls).
  Console getty (rcl2 console mode) spawns a ROOT SHELL with no
  authentication — by design (physical access = root). serial-getty only
  starts a shell if the UART exists.
- Trust boundary: /etc/rcl2.conf is root-owned (initramfs 0644 root; ext4
  root on installed). Config paths execve'd as root — acceptable given
  root-only writability; rescue mode (l2.rescue=1/single/s/emergency) is
  also root, same physical-access assumption.
- busybox ships `getty login passwd su` applets (build/out/busybox.applets)
  but the boot path bypasses them; /etc/passwd is `root:x:0:0` with NO
  /etc/shadow. Behavior of `login`/`su` against missing shadow UNTESTED —
  candidate for mission 67 (empty-password bypass check). No setuid bits
  are set anywhere in build_rootfs (install -m 0755/0644 only).
- Memory-safety posture: C core -Os -Wall -Wextra, fixed buffers with caps,
  Rust parser panic=abort → rcl_c_panic idles PID1 (never exits); signal
  handlers async-safe (flag + nonblocking pipe); no SA_RESTART (EINTR
  paths re-check state). Audit found no obvious overflow in the read
  sections; deep fuzz of the Rust config parser = mission 67 (L2-ea W1
  has ABI limits: Svc=507/Conf=8692, name24/path96/arg64/env64, 16 svcs).

## 63 — package signature validation (audited → gapmap §46)

None. sha256 pins = integrity; store name = archive hash; TOFU trust gate
covers remote RECIPE EXECUTION only (show+confirm/--trust, content-hash
store, non-TTY refuses). `install <file.l2p>` has no authenticity check.
Implementation lane: L2-ea (design-only for now per their message).

## 64 — filesystem permissions (audited 22:05, build/rootfs staging)

- VERIFIED CLEAN: no setuid/setgid binaries (`find -perm /6000` empty);
  no world-writable dirs without sticky bit; /etc/passwd + /etc/group 0644;
  /etc/l2, /l2/store, /l2/var/db/installed 0755; binaries 0755.
- /init shows 777 = SYMLINK mode (lrwxrwxrwx → bin/rcl2), meaningless ✔.
- No /etc/shadow (by design; root:x in passwd; login/su behavior untested
  → mission 67). /dev EMPTY in staging: build runs non-root so mknod is
  skipped; rcl2 mounts devtmpfs at boot (DEVTMPFS=y, DEVTMPFS_MOUNT=n) and
  opens /dev/console itself — boots verified working. cpio --owner=0:0
  normalizes staging maiake:users → root:root in the initramfs ✔.
- HARDENING ITEMS (minor, all-root system): /root is 0755 — recommend
  0700 (build_rootfs mkdir mode; our build-infra lane, deferred batch).
  /tmp is 0755, NOT 1777 — convention deviation; interacts with maiake-f6
  finding (d) (rcl2 live-mode mounts /tmp as UNCAPPED ramfs → OOM/respawn
  wedge): fix should be tmpfs with size= cap AND mode 1777 in rcl2
  mounts_setup — L2-ea lane (rcl2), tracked under missions 19/29/93.
- Store immutability is by CONVENTION (root-writable dirs); tamper is
  detected by l2pm verify, not prevented by permissions (gapmap §50;
  content-addressed naming makes silent replacement detectable).
- OPEN QUESTION (mission 51 pending review): host-path `l2install dir`
  copies build/rootfs staged as maiake:users — if cp -a preserves uid,
  installed files could carry the build user's uid. Live/disk install
  path is root:root (initramfs source) ✔; host dir path needs verification.

## 66 — subprocess execution (partial)

- l2pm: recipe commands via `/bin/sh -e -u -c`, fixed env (DESTDIR/PREFIX/
  SRC/SOURCE_DATE_EPOCH/JOBS/CC/TZ/LC_ALL), umask 022; git ref charset +
  option-injection guard (C:1188–1194); mutual-exclusion validation.
- l2install: destructive disk ops behind typed confirmation / --yes;
  find_medium requires L2-LIVE (good).
- l2net: modprobe chain = F1 (above); N3 (L2-ea finding): `modprobe -a -q
  $_list` unquoted in probe_modaliases — intended word-split, exotic glob
  risk, `set -f` hardening queued in their W6.
- udhcpc.script: CLOSED — L2-ea W6 audit CLEAN, lead spot-check VERIFIED
  (router/dns/domain charset-validated, domain explicitly untrusted,
  resolv.conf mktemp + atomic mv, ip/mask/broadcast binary-derived quads).
  Micro-notes: fallback tmp.$$ predictable path only if mktemp fails
  (root-only threat, moot); unquoted ${broadcast:+} expansion safe given
  udhcpc's inet_ntoa-derived values. Cite: docs/armageddon-ii-progress.md
  §W6 + this spot-check.
- overlay/bin/shutdown: signals PID1 only — clean.

## 69 — supply chain (partial)

- sources.lock: sha256 pins for all components; NO http:// URLs in the lock.
- **dash hardcoded http://** in l2:293 (latest resolver), l2:327
  (`l2 update` downloads with EMPTY sha → pins whatever arrives over
  plaintext HTTP = persistent MITM pin), l2:683 (build; uses lock sha).
  Fix lane: build infra (ours) — switch to https mirror + refuse empty-sha
  pinning over http; defer l2 edits until no peer ./l2 build runs.
- Reproducibility: kernel rebuild byte-identical verified this session
  (0fa6cfc8…); l2pm --check double-build hash compare exists; ISO epoch
  pinning + MBR ID pin documented (re-verify under mission 52).
- Remote recipe repos: git pin via L2PM_RECIPE_REPO_REF else origin/HEAD
  tip + warning (TOFU); raw-URL recipes cached, trust-gated. Recommend
  signed repo index long-term (gapmap §47).

## Status / limitations
- 61 audited (SMEP/SMAP/PTI symbol absence needs runtime verification);
  62 audited (login/su shadow behavior untested → 67); 63 audited;
  64 audited (no setuid/no ww-dirs; /root 755→700 ours deferred, /tmp
  755→1777+cap interacts with finding (d) → L2-ea); 66 COMPLETE (udhcpc
  closed, dual citation); 69 partial (https fix queued).
- No fuzzing/fault-injection executed yet (67/68/70 pending). F1 regression
  test pending L2-ea W6 fix. No source changes by this audit.