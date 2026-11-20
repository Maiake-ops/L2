# ARMAGEDDON II — L2-ea workstream ledger (missions 101–300)

Session: **L2-ea**, started 2026-11-20 ~20:05 local. Directive: continue the
100-mission ARMAGEDDON campaign (shared peer ledger: `docs/armageddon-progress.md`,
co-owned by L2-72 + L2-96) with workstreams 101–300. Branch:
`campaigns/armageddon-ii` (from `cd0f88a`, main). No pushes, no history rewrites.

## Multi-session lane split (deconfliction messages sent 20:14)

| Session | Lane | Write scope |
|---|---|---|
| L2-96 | Kernel low-RAM **variant** + boot matrix: NEW `config/kernel.lowram` + `tools/l2-lowram`, separate `build-lowram/` tree, logs `build/campaigns/lowram-*`; **owns `tools/lowram_matrix.py` + test** (corrected per L2-72 msg; still UNTRACKED, not in 1888794); snapshots `build/out/rcl2`+`initramfs-l2` once read-only. Does NOT edit rcl2/l2pm/kernel.fragment/patch-kernel-ram.py/l2/build/out | as listed |
| L2-72 | **Original ARMAGEDDON campaign session** (corrected 21:0x): shared ledger co-owner, mission reports 01-04/05/11-13/21, broad audits/verification; **mission 05 default-kernel trim — its heavy-coder REBUILDS `build/out/{vmlinuz-l2,initramfs-l2,l2.iso,l2-wifi.iso}` in the MAIN tree** (JOBS=6 L2_LOAD=5, checks concurrent ./l2 builds). Cedes rcl2/l2pm implementation to L2-ea; keeps audits. l2pm gapmap incoming: `build/campaigns/armageddon-l2pm-gapmap.md` | `config/kernel.fragment`, `tools/patch-kernel-ram.py` (mission 05), `docs/armageddon*`, `build/campaigns/armageddon-*`, build/out artifacts |
| maiake-f6 | **Worktree session CONFIRMED** (was "other"): `.qwen/worktrees/campaigns`, branch `campaigns/engineering`, base 21031ce (pre-cd0f88a — merge sequencing flagged). Verification/handoff role; owns `tools/test_mempressure.py` (orchestrator-owned: RUN, don't edit) + **l2pm/Makefile alignment fix** (build-path divergence). Never writes main checkout | worktree only |
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
      Amendments: #1 rcl2-scoped tests only (lead runs full make check);
      #2 pinned-snapshot kernel for QEMU + peer mission-21 ABI constraints
      (Svc=507/Conf=8692 asserts, 16 svc/6 args/8 env, name24/path96/arg64,
      no-SA_RESTART/EINTR, async-safe handlers, PID1-never-exits, CGROUPS=n
      → RLIMITs, wait-ordering deliberate) + boot-resident capture (closes
      peer's pending mission-22 re-measure).
- [~] W2 l2pm hardening (41–50, 181–200) — heavy-coder running (background,
      parallel to W1, disjoint scope). **Reprioritized 21:1x (amendment #3)**
      on peer gapmap `build/campaigns/armageddon-l2pm-gapmap.md` + my
      independent verification of all 3 load-bearing findings:
      **P1** gc data-loss chain (DB-only liveness C:2563-2571 + default
      inactive-gen rm_rf C:2586-2588 + zero fsync ⇒ power-loss→corrupt-DB→
      gc deletes LIVE store; torn rollback re-sync C:2541-2545 feeds same
      path) → gen-file liveness always, fail-safe gc, keep-last-N default;
      **P2** fsync ordering + generalize .l2pm-gen re-sync (C:2346-2382);
      **P3** missing-manifest silent pass (C:2258) → warn/reconstruct + fsck;
      P4 transaction rollback, P5 solver edges + link_tree last-wins
      (C:1583-1586) deferrable with handoff. Signatures/binary-repos remain
      design-only. Gapmap replaces W2's own audit task. TDD observed:
      tools/test_l2pm_transactions.py written before l2pm.c edits.
      Amendment #4: **l2pm/Makefile OFF LIMITS** (maiake-f6 owns alignment
      fix: make lacks -ffunction-sections/-fdata-sections/--gc-sections
      that l2:739 uses → different shas, same 88,184 B size). W2 scope:
      l2pm.c, recipes/, tests, scratch. Same-path size comparisons valid.
- [x] W3 commit verified prior-mission artifacts — **1888794** (9 files,
      2369 insertions): 7 unit-test files + patch-kernel-ram.py + this ledger.
      Peers' live files (lowram_matrix*, docs/armageddon-progress.md,
      docs/armageddon/) left untracked for their owners.
- [~] W4 adversarial review (kernel-maintainer hat): cd0f88a ELF audit clean;
      kernel RAM patchset review findings below (read-only, L2-96's lane);
      l2install security audit done (findings below) → fixes queued as W6
- [ ] W5 release/CI integration of new gates (281–300) once W1/W2 land.
      Includes README size-table refresh: "~67 KiB l2pm" claim STALE vs
      88,184 B actual (peer-verified); use final measured sizes post-W2.
      Reproducibility evidence to cite: maiake-f6 worktree 07-reproducible.md
      — ISO assembly byte-reproducible (2 rebuilds from extracted staging,
      both 06a4fd1e… = baseline), cross-checkout-path l2pm compile identical.
- [ ] W7 rcl2 live-mode /tmp OOM wedge (maiake-f6 verified finding; missions
      19/25/29/30 + 101–120 memory-pressure) — **queued as W1 FOLLOW-UP**
      (same agent reuse after W1 lands; W1 mid-edit in main.c). Mechanism
      confirmed: main.c:330-338 mounts /run+/tmp tmpfs ONLY when
      /etc/l2/installed exists → live /tmp uncapped rootfs → 16M guest
      /tmp fill OOM-kills console dash, respawn also killed (~2.6 MiB
      unevictable pages), PID1 survives (76 kB) but console wedged until
      reboot. Fix: size-capped tmpfs /tmp (and consider /run) in live mode
      too — ENOSPC before global OOM; preserve installed-mode behavior +
      live-installer scratch (test-live-install at 256M). Regression gate:
      RUN worktree tools/test_mempressure.py --iso <fixed-rcl2 ISO> --ram
      16M UNEDITED. Evidence: worktree build/campaigns/09-baseline-
      mempressure-16M.log. Private ISO under build/a2-* from pinned
      snapshots (assembly proven reproducible) or coordinated build/out
      window with L2-72. Full handoff packet expected from their auditor.
- [ ] W6 shell safety hardening: l2install + l2net (62/64/66/67/221–240/261–280)
      — queued; reuse a heavy-coder agent after W1/W2 land (credit discipline:
      max 2 concurrent heavy agents). Findings + regression-test scope below.
      udhcpc.script audited CLEAN (no changes needed). Peer F1 write-up:
      docs/armageddon/61-69-security-posture.md (exploit: attacker CD in sr0
      wins loop order over USB boot medium). **Phased fix**: P1 (my lane) =
      L2-LIVE marker requirement + fail-closed pack search + l2install
      F3/F4/F5/F7/F14 + N3 set -f; P2 = pack integrity manifest with hash
      PINNED IN INITRAMFS (boot-chain trust; marker alone is forgeable text),
      verified by l2net before use — needs l2 build-script edits = L2-72
      deferred lane, coordinate after mission 05. MODULE_SIG = kernel lane +
      user decision. Mission-70 regression test: L2-72 writes after W6 lands.

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
- **build/out VOLATILITY protocol (L2-72 mission-05 rebuilds active)**: never
  read live build/out for test evidence — use pinned snapshots
  (`build/campaigns/baseline-artifacts/` + SHA256SUMS, kernel 0fa6cfc8…) or
  own copies under build/a2-* with recorded hashes; sanity-check rootfs copies
  for torn restage races. L2-ea runs no ./l2 build; ping L2-72 before any
  future exception. W1 amended accordingly (21:0x).

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

## Cross-lane findings (recorded, NOT my write scope)

- **dash HTTP MITM-pin (L2-72 build-infra lane, verified by me)**: l2:293
  resolver + l2:327 `l2 update` download with EMPTY sha over http:// →
  pins whatever arrives into sources.lock (persistent MITM); l2:683 build
  uses the lock sha. Fix: https mirror + refuse empty-sha pinning over http.
  Deferred until no peer ./l2 builds run.
- **busybox login/passwd/su/getty + /etc/passwd root:x, NO /etc/shadow**
  (L2-72 mission 67): boot bypasses auth by design (rcl2 console → root
  shell, physical access = root); login/su behavior vs missing shadow
  untested (empty-password bypass check). W1 does NOT touch console/auth.
- **Kernel posture decisions → user/campaign J** (peer §61): CPU_MITIGATIONS
  off (Spectre V1/V2 vulnerable), no SECCOMP/STACKPROTECTOR/MODULE_SIG/LSMs;
  KASLR/STRICT_RWX/IBT/INIT_STACK_ALL_ZERO/VMAP_STACK on. Enabling any
  hardening trades against 10 MiB ISO / 15 MiB idle budgets — measure first.
  W5 release docs must reference this posture honestly.
- rcl2 parser fuzz ownership: W1 (running) owns it; peer mission 67 keeps
  non-rcl2 malformed input; corpus shared on landing (no duplication).

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
  (condition met: committed at HEAD, 71 l2pm tests green — corrected count:
  security 45 + pipeline 19 + validation 3 + features 4; earlier "84"
  misattributed the stability harness's 17).
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
- 21:0x L2-72 replied (it is the ORIGINAL campaign session, not the matrix
  author): matrix tools are L2-96's; L2-72 owns mission-05 default-kernel
  trim and IS rebuilding build/out in the main tree; cedes rcl2/l2pm
  implementation to me, keeps audits; shared ledger Concurrency section —
  L2-72 adds my lane (I avoid concurrent edits). Answered: split stands,
  my directive is additive not whole-campaign. W1 amended (#2): pinned
  baseline-artifacts kernel for QEMU, peer ABI constraints relayed,
  boot-resident capture added. Read peer audit docs/armageddon/21-rcl2-
  lifecycle.md (mission 21 DONE; gap map 22-30; CGROUPS=n ⇒ RLIMITs).
  Correction sent: cd0f88a was committed on main, branch created after.
- 21:1x L2-72 delivered l2pm gapmap (missions 41-50, file:line evidence).
  Lead independently CONFIRMED: gc DB-only liveness + default gen deletion
  + zero fsync = power-loss→live-store-deletion chain; verify legacy-manifest
  silent pass; link_tree last-wins. W2 amended (#3): P1 gc safety, P2 fsync
  ordering/.l2pm-gen truth, P3 fsck+manifest, P4/P5 deferrable; gapmap
  replaces its audit task. Ack sent to L2-72 with re-verification results.
  Agent progress observed via git status (not transcripts): W1 wrote
  rcl2/verification/test-truncation.c + test-lifecycle.c, Makefile touched;
  W2 wrote tools/test_l2pm_transactions.py (l2pm.c still pristine).
- 21:2x L2-72 actioned all points; my l2net finding recorded as F1 (HIGH)
  in docs/armageddon/61-69-security-posture.md with sr0-ordering exploit.
  W6 fix shape accepted WITH PHASING (P1 shell-side mine; P2 initramfs-
  pinned pack manifest needs their l2 lane post-mission-05; marker alone
  forgeable). Mission-70 regression theirs after W6. Verified dash HTTP
  empty-sha MITM-pin (l2:293/327/683) — their lane. Closed their §66
  udhcpc pending item with my clean audit. Fuzz ownership split: W1 rcl2
  parser, mission 67 login/su/shadow + rest. Test count corrected to 71
  (8a582c9). W1/W2 still running.
- 21:3x maiake-f6 identified (worktree owner, verification/handoff lane).
  Two findings CONFIRMED in main-tree code: (1) rcl2 live /tmp+/run tmpfs
  mounts gated on INSTALLED_MARK (main.c:330-338) → 16M OOM-wedge defect
  (their test_mempressure.py evidence) → W7 queued as W1 follow-up, run
  their test unedited as gate; (2) l2pm build-path flag divergence
  (l2:739 vs Makefile) → W2 amendment #4: Makefile off-limits, their fix
  stands. Branch-topology merge note flagged (worktree base 21031ce).
  Their ISO-assembly reproducibility evidence recorded for W5. W1 already
  hardening /proc/mounts read path (observed via code comment, not
  transcript). Acks sent both directions.