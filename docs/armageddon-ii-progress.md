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
- **REFINED FLOORS (L2-96 default-ISO matrix, harness commit e24248c on
  campaigns/armageddon-ii, 39 unit tests OK)**: BIOS floor **14M** (13M
  OOM); UEFI floor **(96M,128M]** (REFINED 22:5x: maiake-f6 tested 96M
  FAIL + 48M panic, same Limine init_memmap high-memory-allocator panic
  class; earlier (64M,128M] bracket superseded; worktree log citations
  in L2-96 report). Bootloader-layer constraint, not kernel. 16M smoke:
  used 5560K. Evidence: build/campaigns/lowram-default-matrix/. <10M
  BIOS target still open — L2-96 WP2 kernel+ISO BUILT, verification in
  flight; will report which lever broke the 7490 KiB wall.

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

- [x] W1 rcl2 lifecycle hardening (21–30, 141–160) — **DONE 22:3x, lead
      review APPROVED, committed**. file_read EFBIG probe + per-caller
      policies (config→default shell, cmdline→complete-token trim,
      mounts→streaming scanner); parser rejects NUL lines/>6 args/overlong
      args (two-pass); **pre-existing rescue bug fixed** (is_ws lacked
      '\n' → last-token l2.rescue=1 never matched). Measurements: binary
      17,184→18,656 B (+8.6%), selftest RSS 48 KiB, **boot-resident 40 KiB**
      (was 68 — closes mission 22). QEMU PID1 proof on pinned kernel
      0fa6cfc8: L2# 12.2 s, clean poweroff. 7/9 C groups + 4 harness groups
      fail-before; 14/14 after. History below.
      Original brief (history): truncation semantics of
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
- [x] W2 l2pm hardening (41–50, 181–200) — **DONE 22:3x, lead review
      APPROVED, committed**. P1 fail-safe gc (refuse on unreadable DB/gen;
      liveness DB+kept gens; keep active+newest 2/profile;
      --prune-generations legacy opt-in) closes the live-store-deletion
      chain; P2 fsync file+dir at every temp+rename (19 sites); P3
      verify --repair (intent-marker rollback completion, DB-from-gen
      reconstruction, missing manifest never passes, divergence rebuild;
      store NEVER modified); P4 transactions add/install/remove (restore
      DB+profile on failure, set-aware multi-remove); P5 dep-version +
      profile-conflict warnings. 25/33 new tests fail-before, 104/104
      after. Binary 88,184→100,472 B (+12 KiB ≤ cap), RSS 168–172 KiB.
      Single-writer assumption documented; concurrency/power-loss stress
      handed off. Signatures/binary-repo design proposals in report.
      History below.
      Original brief (history): reprioritized 21:1x (amendment #3)
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
      **Layer caveat (maiake-f6)**: that proof covers ASSEMBLY only
      (xorriso + limine-bios-install + MBR-pin given identical staging);
      compile-layer determinism double-build proof still pending (their
      campaign 7). W7 private ISO + W5 claims must pin EVERY staging input
      hash (a2-baseline-artifacts.txt + baseline-artifacts/SHA256SUMS +
      fixed-rcl2 hash), not assume compile reproducibility.
      **FINAL sequencing (L2-96 coord, 23:3x)**: at FINAL, L2-ea refreshes
      build/out/rcl2 with the final committed post-W7 build (my lane),
      coordinated with L2-72's rebuild window (ping protocol); hash+size
      recorded + shared BEFORE L2-96 re-copies into variant initramfs
      assembly + sha pin to maiake-f6. Size nuance corrected: W1 binary
      18,656 B (+1,472 vs build/out's 17,184 — NOT a saving); 40 KiB =
      boot-resident, selftest 48 KiB. Storage win = W7 carries (~14.5-15 KB
      target, ~2-3.7 KiB saved; maiake-f6 worktree final 13,544 B); use
      FINAL measured hash/size for the 10M margin (~550-600K gap).
      build/out/initramfs-l2 regenerated 21:03 (L2-72 batch, their lane);
      peer forensics stay pinned to snapshots. W7 /tmp cap closes L2-96
      variant's inherited WEDGED mempressure expectation.
- [~] W7 rcl2 live-mode /tmp+/run OOM wedge + D5 OOB fix (maiake-f6 verified
      findings; missions 19/25/29/30 + 101–120) — **LAUNCHED 23:2x as W1-agent
      continuation** (warm runtime). Packet read in full + lead-verified:
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
      window with L2-72. **Gate definition LOCKED (maiake-f6 ack)**: run
      tools/test_mempressure.py --iso <fixed-iso> --ram 16M UNEDITED;
      pass = ENOSPC-at-cap + shell survival + reclaim (three-part).
      Their auditor's handoff packet (incoming via send_message) covers
      /tmp AND /run, cap rationale, installer-scratch constraints, idle-
      cost deltas, line refs re-aligned against CURRENT main-tree main.c
      (their worktree base predates cd0f88a + W1 edits).
      **Launch scope**: H1 sysinfo caps (/tmp 12.5% clamp 2-16 MiB, /run
      6.25% clamp 1-8 MiB; installed branch byte-identical; floor
      degradation; ksysinfo layout + scratch_opt bounds lead-verified);
      H2/D5 console_has_ttys OOB (CWE-125) — lead traced repro
      craft="x\0\0ttyS0" i=7 window past NUL → wrong TERM; strlen-bounded
      fix + ported repro with attribution. Carries ACCEPTED conditional on
      full re-verification: -Oz + force-unwind-tables=no (−3.7 KB, no-CFI
      trade documented), rss_report g_iobuf reuse (−256 B bss), probe-byte
      refinement (whitespace-only probe ⇒ non-truncation: max-length
      cmdline/config parse fully; EFBIG kept for genuine cuts; stop-set
      aligned to is_ws), selective test-supervision ports (iobuf_aliasing,
      conf_exact_fit, console_ttys; conf_truncated adapt/drop). Gate:
      their test_mempressure.py UNEDITED on private ISO (pinned staging)
      + test-live-install 256M + idle delta ≤~64 KiB. §10 heavy-test
      follow-ups routed to verification tier; README → W5.
- [~] W6 shell safety hardening: l2install + l2net (62/64/66/67/221–240/261–280)
      — **LAUNCHED 22:4x** (heavy-coder, background; P1 scope, 8 findings,
      fail-before/pass-after required; P2 deferred per co-signed contract).
      udhcpc.script audited CLEAN (no changes needed). Peer F1 write-up:
      docs/armageddon/61-69-security-posture.md (exploit: attacker CD in sr0
      wins loop order over USB boot medium). **Phased fix**: P1 (my lane) =
      L2-LIVE marker requirement + fail-closed pack search + l2install
      F3/F4/F5/F7/F14 + N3 set -f; P2 = pack integrity manifest with hash
      PINNED IN INITRAMFS (boot-chain trust; marker alone is forgeable text),
      verified by l2net before use — needs l2 build-script edits = L2-72
      deferred lane, coordinate after mission 05. MODULE_SIG = kernel lane +
      user decision. Mission-70 regression test: L2-72 writes after W6 lands.
      **P2 design caveat (L2-72, accepted)**: installed-disk normal boot has
      NO initramfs → pin must ALSO live on the ext4 root, written by
      l2install (my lane) at install time from the verified medium.
      Resurrection case: if /lib/modules/$KREL is missing on an installed
      root (kernel upgrade/corruption), find_pack falls through to medium
      search → root-side pin required, not just live-boot protection.
      Format contract sketch (agree with L2-72 before P2 implementation):
      medium `l2/pack-manifest` = sorted "<sha256>  <relpath>" lines over
      pack files (build side writes, deterministic); pin `/etc/l2/pack-hash`
      = sha256 of the manifest; live: embedded in initramfs at ISO build;
      installed: l2install copies from verified medium; l2net: marker →
      manifest-hash vs pin → per-file hashes of used files → fail-closed.
      Open: one manifest vs per-pack; host-install pin source = build staging.
      **CONTRACT CO-SIGNED (22:2x, docs/armageddon/p2-pack-pin-contract.md)**
      with 3 amendments sent for incorporation: A1 uniform verify-ALL-pack-
      files-at-FIRST-activation (closes boot-time firmware gap: load_modules
      probes all modaliases → wifi pack activates at boot when wifi HW
      present, iwlwifi/r8169 request firmware at probe; ~1s one-time wifi
      cost, /run cache after); A2 verifier-side manifest parse hardening
      (reject absolute/..//duplicate relpaths/bad hashes/CRLF — crafted
      medium controls manifest bytes); A3 upgrade invariant (any l2net
      replacement on installed systems must refresh pin; P2 shell+build
      ship same release; old ISOs unaffected via immutable initramfs).
      Per-pack manifests + dual pin + fail-closed + build flow ACKed.
      Mission-70 additions suggested (wifi-HW boot cost, malformed manifest,
      upgrade-without-refresh). P2 shell implementation waits for L2-72
      mission-05 ping. **FINAL ACK 23:0x**: doc header records CO-SIGNED
      (L2-ea 22:20, A1-A3) + lead acceptance 22:25 + A2 extension
      (symlink/non-regular-file rejection — busybox sha256sum follows
      symlinks, rejection closes traversal). Status AGREED; nothing
      pending from my side. **A1 enforcement point (lead P2 shell-side
      note)**: verification runs in use_pack BEFORE any modprobe/exec/
      firmware-path exposure. A2 ext: pack tree regular files+dirs only,
      symlinks/devices/FIFOs rejected upfront. Mission-70 cases include
      wifi-HW boot cost, malformed-manifest+symlink refusal,
      upgrade-without-refresh.

Mission mapping 101–300: kernel/boot (101–140) → L2-96; rcl2 (141–160) → W1;
l2pm (181–200) → W2; build system (201–220) → W2/W5; networking/userland
(221–240) → backlog; QEMU matrix (241–260) → L2-72; security (261–280) → W4
+ backlog; release/docs (281–300) → W5.

## Ground rules (inherited from ARMAGEDDON ledger + directive)

- Every claim needs command + output; logs `build/campaigns/a2-*`.
- Small reviewable diffs; heavy-coder implements, L2-ea reviews and commits.
- **COMMIT-REFUSAL POLICY (lane dispute, 23:0x)**: no fragment/patch-script
  (or any lane-owned) commits from non-owning sessions mid-iteration.
  Lane work is committed only when its owning session declares it
  verified and ready, with attribution, after lead review. Only the USER
  reassigns lanes.
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

**OUTCOMES — L2-96 tested all six (22:1x; evidence: build/campaigns/
lowram-forensics-report.md §Adversarial review + build/lowram-forensics/):**
1. CONFIRMED: 4G guest pcp high=0 batch=7, min_free=128; l2net's
   percpu_pagelist_high_fraction=100000 CANNOT re-enable caching
   (zone_highsize returns 0 under BASE_SMALL). 800-fork burst 0.8 s, zero
   alloc failures — light loads fine; 16G heavy-throughput → mission 80.
   Recommendation to mission-05: runtime-gate on totalram_pages. ENDORSED.
2. QEMU S3 (RTC wakealarm) + SMP2 + poweroff + reboot clean at 256M;
   real-firmware wakeup/hotplug = residual risk (documented).
3. CONFIRMED zero-tail invariant (<128 MiB guests only; inert above).
   Exposure small (PROC_PAGE_MONITOR/page_idle/HWPOISON/CMA/balloon off).
   Recommendation: map tail page READ-ONLY so stray writes fault again —
   zero RAM cost. ENDORSED for mission-05.
4. 8K stacks: VMAP_STACK=y; test-stability --ram 16,32 PASS (churn 1000,
   fanout, 4 MiB stress, ext4-on-virtio write/clean-reboot/recovery,
   console restart, no zombies). Resolved at tested depth.
5. RESOLVED — streaming free sound (rd_fill advances initrd_start=free_end;
   final free frees unconsumed tail only; 32M accounting exact: 26 msgs,
   412K = PAGE_ALIGN(421275)). **CORRECTION TO MY NOTE ACCEPTED**:
   retain_initrd=1 is a SILENT NO-OP by kernel design (handler `if (*str)
   return 0`, initramfs.c:636) — only the BARE flag works; verified: 0
   streaming frees, MemTotal −416K, /sys/firmware/initrd present 421275 B
   mode -r--r-----. My "8×16K oddity" was a truncated capture window.
6. RESOLVED benign: alloc_low_pages msg = memory_map_top_down() failing to
   get a 2 MiB-aligned top block (initrd occupies it at 10M) → documented
   ALIGN_DOWN+BRK fallback; extra early 4K PTEs, NO managed-page loss.

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
- **F15 (low, verified via build/out/busybox.applets): `image` mode is
  host-only** — shipped busybox lacks truncate AND losetup (util-linux).
  Current `need truncate` error says "run: l2 deps install" (misleading
  inside the live guest); losetup failure is a generic die. W6: explicit
  host-only guard + clear message pointing live users to `disk` mode.
  (Peer finding (f), L2-72 relay; applet list checked by lead.)

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
  **JOINT POLICY AGREED (L2-96, 22:5x)**: kernel-side runtime gating
  (their lane, post-WP2 measured batch) and l2net boot()'s runtime writes
  (min_free_kbytes=128, lowmem_reserve_ratio=0, pcp fraction=100000) must
  share ONE threshold policy and LAND TOGETHER, else they fight on big
  guests. Their design sketch: boot-time l2_small_ram flag
  (__ro_after_init) for runtime hooks (zone_highsize/batch), direct
  totalram_pages checks in __init hooks. Compile-time constants
  (MIN_THREADS, THREAD_SIZE_ORDER) cannot be runtime-gated — 8K stacks
  stay at all sizes, justified by VMAP_STACK guards + 16M/32M stability
  PASS (closes W4 angle-4 residual). W6-P1 deliberately EXCLUDES boot()
  tuning; the joint l2net change is a queued post-WP2 item (W5-era),
  implemented against their finalized policy — no premature divergence.
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
- **Lowram variant security property (L2-96)**: variant kernel sets
  MODULES=n → no module loading at all, no hw pack on l2-lowram.iso →
  ring-0 module-injection surface (F1 class) removed entirely for that
  variant. Default kernel keeps MODULES=y → W6 marker gate + P2 pin
  contract remain necessary there. W5 docs: two kernels, two postures.
- **Fragment WIP attribution clarified to L2-96**: uncommitted
  config/kernel.fragment (+32) is L2-72's mission-05 work, NOT L2-ea's
  (never edited it); L2-96's kernel queue must coordinate with L2-72
  before verifying/touching it.
- rcl2 parser fuzz ownership: W1 DONE — corpus committed at 1984055
  (tools/test_rcl2_lifecycle.py), shared with L2-72 for mission 67
  (login/su missing-shadow + non-rcl2 inputs remain theirs).
- udhcpc.script: mission 66 CLOSED with dual citation (my §W6 audit +
  L2-72 lead spot-check). Moot micro-notes recorded: predictable tmp.$$
  fallback only on mktemp failure (root-only threat); unquoted
  ${broadcast:+} safe (inet_ntoa-derived). No changes planned (clean).
- **MERGE FLAG (l2net)**: maiake-f6 fixed `l2net status` exit code in its
  WORKTREE (exited 1 without /etc/resolv.conf; if-form, proven with shipped
  dash). W6 (main tree) touches find_pack/use_pack/probe_modaliases ONLY —
  NOT status() — so the two fixes are function-disjoint and should merge
  cleanly; coordinate at merge time (user decision pending).

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
- 21:4x L2-72 closures: F1 phasing agreed; P2 caveat accepted (installed
  boot has no initramfs → l2install propagates pin to ext4 root;
  /lib/modules-missing fall-through resurrects risk on installed systems).
  Format-contract sketch recorded, agreement before P2 implementation.
  Mission 66 closed dual-cited; 67 split recorded (W1 corpus share pending
  landing); dash fix theirs deferred; they re-stamp W1/W2/W6 post-commits
  (mission 92). W1/W2 still running.
- 21:5x maiake-f6 ack: W7 gate locked (ENOSPC-at-cap + shell survival +
  reclaim, their test unedited); auditor packet will cover /tmp+/run with
  re-aligned line refs; their agent C exclusively owns l2pm Makefile
  alignment (hash-identity verification of both paths); their fuzz auditor
  will CONFIRM/REFUTE gapmap at HEAD + adversarially review W2 P1-P3
  commits read-only. Assembly-vs-compile determinism caveat recorded
  (pin all staging hashes). **Merge topology = PENDING USER DECISION**
  (campaigns/engineering base 21031ce vs main cd0f88a+my branch; peer
  will not rebase without direction; surfaced to user, no unilateral
  merge). W1/W2 still running.
- 22:0x L2-72 FYI: maiake-f6 worktree fixed l2net status() exit code →
  merge flag recorded (W6 stays out of status(), function-disjoint).
  Finding (f) VERIFIED by lead against busybox.applets (no truncate/
  losetup) → F15 added to W6 (host-only guard + clear message). Finding
  (d) = W7 already queued; their fix shape matches (tmpfs size= cap in
  mounts_setup). Reproducibility re-confirmation recorded (assembly
  06a4fd1e, cross-path l2pm efc32819, musl wrapper paths don't leak).
  W1/W2 still running.
- 22:1x L2-96 tested ALL SIX W4 angles: 1/3 confirmed (recommendations:
  runtime-gate pcp/min_free on totalram_pages; RO vmemmap tail page —
  both endorsed for mission-05), 2/4/5/6 resolved with evidence; MY NOTE
  CORRECTED (retain_initrd=1 no-op by design, bare flag verified; 8×16K
  was truncated window). Refined floors recorded: BIOS 14M, UEFI
  (64M,128M] w/ Limine 64M panic. e24248c (their verified matrix harness)
  landed on campaigns/armageddon-ii via main-tree commit — branch now
  carries both sessions' verified commits; main still cd0f88a; merge
  topology still user decision. Git status shows all lanes active:
  W1 in rcl2 (incl syscalls.h), W2 NOW EDITING l2pm.c + security suite,
  L2-72 in kernel.fragment, L2-96 in build-lowram/. No intervention needed.
- 22:2x P2 contract (docs/armageddon/p2-pack-pin-contract.md) read in full
  and CO-SIGNED with amendments A1-A3: A1 closes boot-time firmware gap
  (wifi pack activates at boot via load_modules when wifi HW present —
  uniform verify-all-at-first-activation rule replaces subtree split);
  A2 verifier-side manifest parse hardening; A3 pin-refresh upgrade
  invariant + same-release deployment ordering. Mission-70 additions
  suggested. P2 shell-side waits for mission-05 ping; W6-P1 remains
  immediately launchable on agent-slot free. W1/W2 still running.
- 22:3x **W1+W2 INTEGRATION GATE PASSED**. Lead independent review: full
  rcl2 diff (EFBIG semantics, streaming scanner chunk-boundary state,
  two-pass parse 4-token skip, NUL skip, rescue fix) APPROVED; full l2pm
  diff (1243 lines: fsync ordering, txn fail-closed snapshots/reverse
  abort, gc refuse-before-delete + keep-rank logic, repair never touches
  store, security-test adaptation preserves intent) APPROVED. Re-ran
  myself: make -C rcl2 test, 14+33 harness tests, make check EXIT=0.
  Evidence spot-checks: 25/33 fail-before log, boot log resident 40 KiB +
  Power down, 104 OK suites. Wired both harnesses into make check
  (166 unit tests + 4 C suites green, a2-make-check-integrated.log).
  Commits: rcl2 (W1), l2pm (W2), Makefile+ledger (wiring) — hashes in
  git log. W6-P1 launching; W7 awaits maiake-f6 auditor packet.
- 22:4x W6-P1 heavy-coder launched (8 findings, fail-before/pass-after
  required). L2-96 'correction to correction' resolved: no dispute —
  1888794 claim was accurate when sent; e24248c already recorded 22:1x.
  Clarified fragment WIP attribution (L2-72's, not mine) to prevent
  cross-lane disturbance. Variant MODULES=n security property recorded
  for W5. W6 running; W7 queued.
- 22:5x L2-96: UEFI floor refined (96M,128M] (maiake-f6 96M/48M tests) —
  ledger updated. N5 JOINT POLICY agreed: one threshold policy, kernel
  gating + l2net writes land together post-WP2 (their l2_small_ram flag
  sketch); 8K stacks justified at all sizes (angle-4 closed). W6-P1
  exclusion of boot() tuning stands; joint item queued W5-era. Merge
  topology re-agreed (branch = campaign record, main = user's call).
  WP2 10M kernel+ISO built, verification in flight — result expected.
  W1/W2 hashes already sent 22:3x (messages crossed). W6 running.
- 23:0x **LANE DISPUTE (L2-72 flag)**: L2-96's post-WP2 queue treats
  L2-72's LIVE fragment WIP as an abandoned handoff to 'verify then
  commit with attribution', contrary to the recorded split (L2-96's own
  20:5x reservation + my 21:05 confirmation + shared ledger). Lead
  verified: no unauthorized commits (HEAD 02cd24b, last 5 mine); WIP
  LIVE (mtime 21:07, +43/−1 grew from +32; L2-72 agent RUNNING, batch1
  EXIT=0; 05-fragment-wip.patch snapshot 21:18). Actions: commit-refusal
  policy recorded (ground rules); L2-96 messaged with evidence + recorded
  split + constructive path (variant-lane batches or post-mission-05
  coordination); L2-72 confirmed refusal + final P2 ACK (A2 extension
  accepted). L2-72 escalated to user; dispute surfaced in L2-ea user
  report — ONLY THE USER REASSIGNS LANES. W6-P1 unaffected, running.
- 23:1x L2-72 co-sign acceptance confirmed (crossed my FINAL ACK): A1-A3
  incorporated, A2 extended (regular files+dirs only), A3 generalized
  (pack-content changes too), mission-70 additions in. A1 enforcement
  point recorded: use_pack BEFORE modprobe/exec/firmware exposure.
  Dispute remains OPEN pending user; L2-72 continues per recorded split,
  will ping at mission-05 landing → P2 build-side starts. Hashes noted
  as durable in ledger + git log (messages may be held for user review).
  W6 running; W7 queued; WP2 verification in flight.
- 23:2x maiake-f6 W7 packet arrived (worktree 04-rcl2.md, commit 0792991).
  Lead read §1-§10 in full; D5 OOB independently traced + confirmed at
  main.c:719-726; H1 patch design verified (ksysinfo layout, option-builder
  bounds, floor degradation, cap rationale vs OOM point). W7 LAUNCHED as
  W1-agent continuation with locked gate verbatim + carries accepted
  conditionally (−Oz/unwind flags, rss reuse, probe-byte refinement).
  Ack sent with verification details + merge reconciliation (D1-D4
  duplicates, my tree canonical). W6 + W7 now both running (2 heavy
  agents, credit cap respected). WP2 10M verification still in flight.
- 23:3x L2-96 FINAL sequencing coord: build/out/rcl2 refresh at FINAL =
  my lane (post-W7 committed build), coordinated with L2-72 window;
  peer re-copies into variant before sha pin. Corrected their margin
  math: W1 is +1,472 B not a saving; W7 carries give ~2-3.7 KiB; boot-
  resident vs selftest RSS distinguished. Branch state mutually verified
  (cooperative de-escalation). W6+W7 running; dispute still user's call.