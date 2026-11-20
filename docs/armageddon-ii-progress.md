# ARMAGEDDON II — L2-ea workstream ledger (missions 101–300)

Session: **L2-ea**, started 2026-11-20 ~20:05 local. Directive: continue the
100-mission ARMAGEDDON campaign (peer ledger: `docs/armageddon-progress.md`,
owned by session L2-96) with workstreams 101–300. Branch:
`campaigns/armageddon-ii` (from `cd0f88a`, main). No pushes, no history rewrites.

## Multi-session lane split (deconfliction messages sent 20:14)

| Session | Lane | Write scope |
|---|---|---|
| L2-96 | Kernel low-RAM **variant** (corrected 20:5x per peer msg): NEW `config/kernel.lowram` + `tools/l2-lowram`, separate `build-lowram/` tree, logs `build/campaigns/lowram-*`; snapshots `build/out/rcl2`+`initramfs-l2` once read-only. Does NOT edit rcl2/l2pm/kernel.fragment/patch-kernel-ram.py/l2/build/out | as listed |
| L2-72 | Reproducible boot matrix tooling (missions 241–260) | `tools/lowram_matrix.py`, `tools/test_lowram_matrix.py` (still UNTRACKED — not in 1888794; correction sent to L2-96) |
| other session | worktree `.qwen/worktrees/campaigns` (branch `campaigns/engineering`, kernel build observed 20:11) — NOT L2-96's per its correction; owner unconfirmed | worktree only; L2-ea does not touch |
| **L2-ea (this)** | rcl2 lifecycle (21–30, 141–160), l2pm (41–50, 181–200), verification/commits | `rcl2/`, `l2pm/`, new `tools/test_rcl2*`, `build/a2-*/`, `build/campaigns/a2-*`, this file |

L2-ea does **not** write `build/out/` kernel/ISO artifacts, `config/kernel.*`,
`build/src/`, or the peers' tool files while their runs may use them.

## Baseline evidence (2026-11-20, this host: 16 cores, KVM, NixOS toolchains)

- `git log`: HEAD `cd0f88a` (user commit of rcl2 memory WIP: opt-level=z,
  `--no-rosegment -z norelro`, CFG_RAW_CAP 8192→2048, CMDLINE_CAP 4096→2048,
  shared `g_iobuf`, bounds-checked Rust parser).
- `make -C rcl2 test`: **PASS** — selftest ok, supervision regression passed,
  **resident 48 KiB** (budget 150), binary **17,184 B** (was 26,904 B).
- `make check` (HEAD + untracked prior-mission tests): running,
  log `build/campaigns/a2-make-check.log`.
- Prior-session sweep (`build/campaigns/armageddon-baseline-sweep.log`, ISO of
  14:39 build): **10M FAIL** (no prompt), **12M FAIL** (OOM panic t=0.375 s),
  16M PASS (5564 KiB used / 8192 KiB MemTotal), 24M PASS (5632 KiB used).
- 10M direct-boot forensics (`build/lowram-forensics/10M-direct-baseline.log`,
  patched kernel): present 9720 KiB, **managed 1152 KiB**, `kthreadd` OOM at
  t=0.52 s. `Memory:` line: **4891K code + 351K rwdata + 1392K rodata + 692K
  init + 164K bss = 7490 KiB kernel image**, 8568K reserved total.
  At 10M QEMU also places ACPI tables above RAM end → ACPI init fails, MP-table
  fallback works (QEMU artifact, not a kernel defect).
- 32M forensics (`build/campaigns/boot-forensics-32M.log`): 22252K/32248K
  available, 9060K reserved; percpu 2 MiB chunk mostly freed after fallback;
  printk ring 8K data + 28K meta.

## Three most important unresolved bottlenecks (evidence above)

1. **Kernel image footprint 7490 KiB** → managed RAM 1152K/3212K at 10M/12M
   guests; OOM before init. Primary target (<10 MiB boot) blocked.
   **Owner: L2-96** (kernel build in progress in its worktree).
2. **Initramfs 426,833 B gzip / ~1.2 MiB unpacked** charged to the tiny managed
   pool during early unpack; carries non-boot binaries (l2pm 88K, l2wifi 63K).
   Owner: L2-96 lane (rootfs/ISO artifacts); L2-ea provides analysis only.
3. **Verification gap**: prior-mission tests/patch untracked, sweep truncated,
   rcl2 buffer-cap commit untested at boundaries, no committed low-RAM
   regression gate. **Owner: L2-ea** (commits + rcl2/l2pm hardening).

## Workstreams (L2-ea)

- [~] W1 rcl2 lifecycle hardening + fault injection (21–30, 141–160) —
      heavy-coder launched 20:21 (background); scope: truncation semantics of
      the committed 2048-byte caps (file_read silently truncates at cap-1,
      main.c:167 — confirmed hazard), g_iobuf lifetime audit, parser fuzz,
      crash/reap/signal/shutdown harness, private-initramfs QEMU boot proof.
      ELF segment audit already done by lead: CLEAN (R E .rodata+.text, RW
      .data+.bss, GNU_STACK RW — no W+X). config/rcl2.conf = 538 B.
- [~] W2 l2pm transaction/consistency hardening (41–50, 181–200) — heavy-coder
      launched 20:3x (background, parallel to W1, disjoint write scope).
      Audit-first: generations/rollback/gc/verify/search ALREADY exist
      (l2pm.c:1639-2552); real gaps = multi-package transaction atomicity,
      DB/profile torn-state recovery, solver edges; signatures + binary repos
      = design proposals only. W1 amended: rcl2-scoped tests only; lead runs
      full make check after both land.
- [x] W3 commit verified prior-mission artifacts — **1888794** (9 files,
      2369 insertions): 7 unit-test files + patch-kernel-ram.py + this ledger.
      Peers' live files (lowram_matrix*, docs/armageddon-progress.md,
      docs/armageddon/) left untracked for their owners.
- [~] W4 adversarial review (kernel-maintainer hat): cd0f88a ELF audit clean;
      kernel RAM patchset review findings below (read-only, L2-96's lane);
      l2install security audit done (findings below) → fixes queued as W6
- [ ] W5 release/CI integration of new gates (281–300) once W1/W2 land
- [ ] W6 shell safety hardening: l2install + l2net (62/64/66/67/221–240/261–280)
      — queued; reuse a heavy-coder agent after W1/W2 land (credit discipline:
      max 2 concurrent heavy agents). Findings + regression-test scope below.
      udhcpc.script audited CLEAN (no changes needed).

Mission mapping 101–300: kernel/boot (101–140) → L2-96; rcl2 (141–160) → W1;
l2pm (181–200) → W2; build system (201–220) → W2/W5; networking/userland
(221–240) → backlog; QEMU matrix (241–260) → L2-72; security (261–280) → W4
+ backlog; release/docs (281–300) → W5.

## Ground rules (inherited from ARMAGEDDON ledger + directive)

- Every claim needs command + output; logs `build/campaigns/a2-*`.
- Small reviewable diffs; heavy-coder implements, L2-ea reviews and commits.
- No destructive ops, no remote changes, no history rewrites.
- Budgets: ISO < 10 MiB; idle RAM 10–15 MiB band (ceiling 15360 KiB) at any
  guest size; rcl2 PID1 ~50 KiB resident; <10 MiB guest boot = primary target.
- RAM measurements: real ISO, -smp 2, settle 40 s (direct boot reads ~1 MiB
  high; re-run over-budget readings with --settle 40 before calling regression).

## W4 review notes — tools/patch-kernel-ram.py (independent-maintainer pass)

Direction is sound (16M/24M pass with it; streaming initrd free + sync unpack
attack the right peak). Falsification angles sent to L2-96 (its lane):

1. BASE_SMALL is compile-time: pcp high=0/batch=1 and min_free_kbytes=128 floor
   apply at ALL RAM sizes — a 16 GiB machine gets per-CPU page caching disabled
   and tiny atomic reserves. Idle-band tests don't catch the perf/atomic-failure
   risk. Consider runtime gating on totalram_pages for big machines.
2. realmode hunk skips memblock_reserve(0, SZ_1M) (~+639 KiB managed at 10M).
   reserve_real_mode() covers the SMP trampoline separately, but ACPI S3 wake
   paths assume low memory: needs explicit -smp 2 boot + suspend/resume proof
   on real firmware before shipping.
3. vmemmap shared zero tail page: pfn_to_page() beyond max_pfn returns a zeroed
   struct page instead of faulting — masks bugs; every path must pfn_valid()
   first (kpageflags, hotplug). Document the invariant.
4. 8K thread/IRQ stacks: CONFIG_VMAP_STACK=y verified in build/out/kernel.config
   (guard pages present) — still stress deep paths (ext4 writeback, net RX,
   module load) for overflow.
5. Streaming initrd free relies on gunzip never re-reading consumed input
   (true for inflate). Verify retain_initrd=1 path and final free_initrd
   accounting — forensics showed 8× "Freeing initrd memory: 16K", odd.
6. 10M log: "Failed to release memory for alloc_low_pages()" even with
   EARLY_DYNAMIC_PAGE_TABLES=16 — early pgt reserve still fails at 10M;
   investigate its managed-page cost.

## W6 audit — l2install (read-only; missions 62/64/66/67/261–280)

Confirmed findings (none covered by tools/test_l2install.py's 10 tests):

- **F3 (main, medium sev): silent live-medium tool substitution on hosts.**
  setup_live triggers whenever the build tree is absent — including on a
  normal host. find_medium then mounts ANY iso9660 medium carrying
  `l2/L2-LIVE` and prepends its `l2/install/bin` to PATH, so sgdisk/mkfs/
  limine from an inserted USB run as root. README promises host tools on
  the host path. Fix: gate substitution on actually booted L2 —
  `[ "$(cat /proc/1/comm)" = rcl2 ]` (spoof-resistant: attacker would need
  PID 1) — or an explicit flag/confirmation naming the medium. Defense in
  depth: pass `--no-absolute-filenames` to the rootfs cpio extraction
  (GNU+busybox both support it; default strips leading `/` but `..`
  handling is version-dependent).
- **F4 (medium): `image` silently truncates an existing regular file.**
  install_image has no existence/size check; `truncate -s` destroys prior
  contents, and YES=1 is forced for the loop install. Fix: refuse existing
  non-empty targets without --yes (regression test).
- **F5 (low): cleanup() umounts $MNT unconditionally.** With MNT env
  override + early failure it can umount a mount the installer never
  created. Fix: track mounted-flags, umount only what we mounted.
- **F7 (low, exotic): picker line-protocol injection via USB MODEL string.**
  list_disks passes lsblk MODEL through printf '%s|' — embedded newline/`|`
  can inject fake menu lines. Fix: sanitize model with tr -d '\n\r|'.
- **F14 (low): medium mount leak on the host path.** cleanup() never
  umounts $MEDIUM (fine on live, leaked on hosts). Fix: umount when !live.
- Note only: disk_busy misses raw openers (qemu/dd holding the disk without
  mounts); typed confirmation mitigates.

Falsified hypotheses (verified, NOT defects — do not "fix"): --efi-size and
--hostname ARE validated at parse time (l2install:577-580) and preflight
checks ESP+64 MiB against disk size BEFORE zap-all; /sbin/rcl2 resolves via
sbin→bin symlink; GNU/busybox cpio strip leading '/' by default.

## W6 audit — l2net + udhcpc.script (read-only; missions 221–240/261–280)

- **N1 (main, medium sev): pack trust weaker than l2install's.** l2net
  `find_pack` accepts ANY iso9660 medium carrying `l2/wifi` or `l2/hw`
  dirs — no `l2/L2-LIVE` marker check (l2install's find_medium requires it).
  `use_pack` then symlink-injects `/lib/modules/$KREL` from the medium and
  sets it as the firmware path, and runs `$PACK/bin/wpa_supplicant` as root:
  a crafted USB inserted during a core-ISO live boot yields **ring-0 module
  loading** from non-boot media. Installed systems are safe (early return on
  /lib/modules/$KREL). Fix direction: require the L2-LIVE marker AND prefer/
  verify the actual boot device; document the trust model. Shared with
  l2install F3 as one trust-boundary workstream.
- **N3 (low): `modprobe -a -q $_list` unquoted** (probe_modaliases) — dash
  pathname expansion applies; modalias strings are kernel-controlled so
  exploitation is exotic, but `set -f` around the split is free hardening.
- **N5 (info, pairs with patchset review #1): boot() forces
  min_free_kbytes=128, watermark_scale_factor=0, lowmem_reserve_ratio='0 0 0'
  at ALL RAM sizes on installed systems too.** Tiny atomic reserves on big
  machines; zero DMA32 lowmem reserve relies on 64-bit DMA + dynamic SWIOTLB
  — real-hardware 32-bit-DMA devices under pressure need a fault-injection
  test (mission 89). Consider scaling with MemTotal.
- **udhcpc.script: CLEAN.** router/dns/domain charset-validated (domain
  explicitly treated as untrusted), resolv.conf via mktemp+atomic mv,
  ip/mask/broadcast are binary-derived dotted quads from udhcpc, quoted.
  net.conf 644 root-owned → sourcing is not a privilege boundary.
- pid_alive comm-checks pidfiles (no stranger-kill), pack mounts ro,
  carrier-gated DHCP with bounded wait + link-down on no-carrier: all good.

## Log

- 20:05–20:12 inspected repo, prior ledger, forensics; verified rcl2 at HEAD;
  identified bottlenecks; discovered 3 peer sessions (list_agents).
- 20:14 created branch `campaigns/armageddon-ii`; sent lane notes to L2-96/L2-72;
  started `make check` baseline (bg_613ef0d2).
- 20:18 `make check` PASS exit 0 (119 unit tests: 17+45+19+3+4+17+10+4, rcl2
  selftest+supervision, l2wifi/l2gpt builds). Baseline artifact sha256 recorded:
  build/campaigns/a2-baseline-artifacts.txt (ISO d850fb81…, rcl2 e6dd58ef…).
- 20:21 launched heavy-coder W1 (background). ELF audit of cd0f88a: clean.
  file_read truncation hazard confirmed (main.c:167, silent cap-1 cut).
- 20:2x W3 commit of verified prior-mission artifacts; W4 patchset review
  notes sent to L2-96.
- 20:3x launched heavy-coder W2 (l2pm transactions/consistency, parallel,
  disjoint scope); amended W1 to rcl2-scoped tests only (lead runs full
  make check after both land). Retired l2pm-phased-pipeline memory
  (condition met: committed at HEAD, 84 l2pm tests green).
- 20:4x l2install read-only security audit complete: F3/F4/F5/F7/F14
  confirmed, 3 hypotheses falsified by verification; W6 queued with
  regression-test scope. Target status: rcl2 48 KiB resident = ~50 KiB
  target MET; ISO 8.38 MiB < 10 MiB gate MET; <10 MiB guest boot BLOCKED
  by 7490 KiB kernel image (L2-96 lane, kernel build in progress).
- 20:5x L2-96 replied: boundaries confirmed; corrected worktree ownership
  (not theirs) and its actual lane (kernel.lowram variant, build-lowram/).
  Verified + corrected their claim that lowram_matrix.py was in 1888794
  (it is NOT; still untracked). l2net/udhcpc audit complete: N1 pack-trust
  finding (ring-0 via non-boot medium), N3/N5 hardening notes; udhcpc.script
  clean. W6 scope widened to l2install+l2net. W1/W2 agents still running.