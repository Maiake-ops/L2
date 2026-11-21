# ARMAGEDDON II — L2-ea workstream ledger (missions 101–300)

> **🎯 PRIMARY TARGET ACHIEVED (variant): WP2 v3 BOOTS AT 10 MiB** — PASS,
> MemTotal 5440 KiB, used 3836 KiB, spawn ok, kernel 7.3.3-l2-lowram
> (build-lowram/campaigns/v3-10M/, committed by user). v1/v2 10M FAIL
> (oom-deadlock) → v3 PASS. Default-kernel floor still 14M (mission-05).
>
> **RESTORATION (2026-11-21)**: session interrupted/restored; I am now
> **L2-aa** (ref eaaab2, was L2-ea). User rebased + merged main + PUSHED
> the branch during the interruption: HEAD `1910fe1e7` (merge),
> `origin/campaigns/armageddon-ii`, backups `backup/armageddon-ii{,-pre-merge}`.
> My commits rebased to NEW hashes: W1 1984055→`e34ab3d43`, W2 4acfd36→
> `264d82b86`, wiring 1ec7140→`90a887c79`, notes 6b7e296→`ab303b3f2`,
> audit-gate ec827b1→`7f4075f47`, dispute dd32ca7→`0d21a1cae`. **W7 fixes
> + W6 TEST HARNESS** committed by user (`d1959a232`); W6 SCRIPT fixes were
> NOT committed (byte-identical to pre-fix HEAD) — applied post-resume,
> committed `ac62c0101`. Agents PAUSED→RESUMED→COMPLETED. **make check now
> EXIT=0 GREEN** (181 unit tests + 5 rcl2 C suites) after W6. (Resume-time
> EXIT=2 was W6 incomplete; earlier "EXIT=0" was a wrapper-code misread —
> both corrected.) Peer roster now only **L2-54** (ref
> 96af98 = old L2-96); old L2-72/maiake-f6 sessions gone — agreements below
> remain the reference but contacts changed. `notes/qwen1.txt` created per
> user request (notes/ is gitignored; note/qwen1.txt is the tracked copy).

Session: **L2-ea** (restored as **L2-aa** 2026-11-21), started 2026-11-20 ~20:05 local. Directive: continue the
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
  used 5560K. Evidence: build/campaigns/lowram-default-matrix/.
  **🎯 VARIANT 10M BOOT ACHIEVED — WP2 v3 PASS** (2026-11-21 restoration
  read): v1 10M FAIL (oom-deadlock 0.38s) → v2 FAIL (0.41s) → **v3 PASS**
  (MemTotal 5440 KiB, used 3836, reserved 4800, spawn ok, 53.5s, KVM smp2
  settle40, kernel 7.3.3-l2-lowram). Evidence build-lowram/campaigns/
  v{1,2,3}-10M/ (committed by user a8a2849b5). Primary target met for the
  variant; winning-lever report is L2-96/L2-54's lane. Default-kernel floor
  remains 14M (mission-05 trim may move it). Two kernels, two floors —
  W5 docs distinguish.
  **VARIANT v3-bios SWEEP 4/4 PASS (L2-54, 2026-11-21)**: 12M MemTotal
  7488/used 3812/reserved 4800; 16M 11520/3812/4864; 32M 27648/3816/5120;
  256M 253448/3832/8696 — all spawn ok, **idle ~3.8 MiB FLAT across sizes**
  (well under the 10-15 MiB band; vs default kernel ~5.5 MiB). v3-uefi:
  **128M PASS** (118500/3972/12572), **256M PASS** (247524/3956/14620),
  16M FAIL-silent (Limine firmware floor, expected); poweroff-16M done;
  v3-direct 10M running + **9M probe** (<10M stretch). Variant FINAL
  (standalone l2-lowram.iso re-verification incl. 9M, attribution, repro,
  report) proceeds INDEPENDENTLY of the orphaned default-kernel lane;
  only the INTEGRATED ISO is blocked. W5 docs: variant idle ~3.8M flat is
  a headline efficiency result.

## Three most important unresolved bottlenecks (evidence above)

1. **Kernel image footprint 7490 KiB** (default) → managed RAM 1152K/3212K
   at 10M/12M guests; OOM before init. **RESOLVED for the VARIANT: WP2 v3
   boots at 10M (PASS)** — primary target met on l2-lowram. Default kernel
   still 14M floor; mission-05 trim (L2-72) may lower it. Owner: L2-96/L2-54
   variant lane; default-kernel mission-05 L2-72.
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
      (was 68 — mission 22 CLOSED, dual-confirmed: maiake-f6 auditor
      independent 36-40 kB). QEMU PID1 proof on pinned kernel
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
      **INDEPENDENT AUDIT PENDING (sign-off gate)**: maiake-f6 fuzz auditor C
      reviewing 4acfd36 read-only (fail-before/pass-after re-verify, gc
      liveness edges, fsync ordering windows, --repair invariants, txn
      rollback, kill-9 at each txn phase). 4acfd36 records LEAD verification
      only; FINAL W2 sign-off held for their CONFIRM/REFUTE verdicts.
      Power-loss durability INFERRED from fsync ordering (not fault-injected);
      dm-flakey needs root+device-mapper (feasibility stated honestly);
      kill-9 + ordering analysis = deliverable depth, residual gap documented.
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
- [~] W5 release/CI integration of new gates (281–300) — IN PROGRESS:
      docs/armageddon/release-readiness.md DRAFTED (two-kernel comparison,
      posture, gates, measured sizes, labeled predictions, blockers).
      README product refresh + CI wiring held until final sizes/orphan lane.
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
      **INTEGRATED SEQUENCING (L2-72 mission-92 re-stamp, 00:2x —
      supersedes primary refresh path)**: W6/W7 land+COMMIT FIRST →
      L2-72 mission-05 lands+ping → L2-72 integrated ./l2 build rcl2 l2pm
      + rootfs/initramfs/iso picks up committed W1/W2/W6/W7 (rcl2+l2pm
      binaries; l2install/l2net via rootfs staging) → final ISO carries
      ALL hardening → L2-96 variant re-copy + FINAL re-verification
      (selftest re-stamp) + sha pin to maiake-f6. If W7 lands AFTER that
      rebuild, L2-ea refreshes build/out/rcl2 in coordinated window (ping)
      — no double-refresh conflict. L2-ea pings at W6/W7 commit; L2-72
      pings at mission-05 landing.
      **MISSION-92 RE-STAMP DONE (L2-72 independent)**: 1984055/4acfd36/
      1ec7140 verified; make check EXIT=0 (166+4 suites, evidence
      build/campaigns/92-restamp-make-check.log); sizes match (rcl2
      18,656 / l2pm 100,472); rescue fix + fsync + verify --repair +
      manifest-never-silent-pass confirmed; fragment/patch 0 touches.
      l2pm selftest RSS 168 KiB confirmed (W5: 168 KiB + ~98 KiB binary).
      Boot-40 citation provided (a2-rcl2-boot.log:297, pid1_boot path ≠
      selftest 48); L2-72 boot-40 stamp pending accept/re-measure.
- [x] W7 rcl2 live-mode /tmp+/run OOM wedge + D5 OOB fix (maiake-f6 verified
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
      **DONE 2026-11-21 — LEAD VERIFIED + ACCEPTED** (independent): make -C
      rcl2 test EXIT=0 (5 C suites, 0 warnings, resident 44 KiB, rescue
      fixtures green); harness 14/14. Locked gate PASS (a2-w7-mempressure-
      16M.log: used 5608→5536 reclaim, shell survives, no OOM; a2-w7-enospc-
      16M.log: size=2048k, 32+0 records out = 2048 KiB exact, dd_rc=1).
      live-install 256 PASS (RC=0 + installed boots L2#). idle +20/+44 KiB
      (≤64). boot-resident 36 KiB; caps /tmp 16384 /run 8192 KiB at 256M.
      Binary **15,568 B** (sha 07fb2c69…); ISO build/a2-w7/l2-fixed.iso
      8,781,824 B sha 482e5933… (<10 MiB). All 4 carries kept (-Oz/unwind/
      rss/probe-byte), none regressed; .eh_frame 112 B prebuilt
      compiler_builtins residue (harmless). W7 source committed d1959a232
      (rcl2 tree CLEAN). NOTE: full make check EXIT=2 (W6 l2install red,
      pending) — W7 rcl2 portion green; W7 report's "make check EXIT=0"
      inherited lead's earlier wrong claim, corrected. Gate agent-run
      (worktree tool unedited) + lead log-verified; independent QEMU re-run
      deferred (L2-54 sweep active, one-QEMU discipline). §10 heavy-test
      follow-ups queued; test-live-install.py argparse sync optional.
      rcl2 post-W7 build ready for FINAL variant re-copy (L2-54); default
      integrated ISO build still ORPHANED (user decision).
- [x] W6 shell safety hardening: l2install + l2net (62/64/66/67/221–240/261–280)
      — **DONE 2026-11-21, lead verified + ACCEPTED, committed ac62c0101**.
      All 8 findings fixed (F3/F4/F5/F7/F14/F15 + N1/N3); 25/25 lane tests
      green; fail-before/pass-after logged; full make check EXIT=0 (181
      tests). test_l2net wired into check. P2 deferred per co-signed contract.
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
sbin→bin symlink.
**CPIO CLAIM CORRECTED (W6 empirical, 2026-11-21)**: my earlier "GNU/busybox
both strip leading '/' by default / both support --no-absolute-filenames"
was WRONG. Reality: busybox cpio STRIPS leading '/' by default but REJECTS
--no-absolute-filenames; GNU cpio does NOT strip by default but ACCEPTS the
flag. So F3 cpio containment was a REAL defect on GNU hosts (absolute member
escaped on HEAD — test_f3_cpio fail-before). Fix: runtime support probe adds
the flag on GNU, omits on busybox (live flow byte-identical).

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
  (executor L2-72, missions 05/80/65 — ownership clarified 23:4x post-
  retraction; L2-96 = variant iteration only) and l2net boot()'s runtime writes
  (min_free_kbytes=128, lowmem_reserve_ratio=0, pcp fraction=100000) must
  share ONE threshold policy and LAND TOGETHER, else they fight on big
  guests. Their design sketch: boot-time l2_small_ram flag
  (__ro_after_init) for runtime hooks (zone_highsize/batch), direct
  totalram_pages checks in __init hooks. Compile-time constants
  (MIN_THREADS, THREAD_SIZE_ORDER) cannot be runtime-gated — 8K stacks
  stay at all sizes, justified by VMAP_STACK guards + 16M/32M stability
  PASS (closes W4 angle-4 residual). W6-P1 deliberately EXCLUDES boot()
  tuning — no premature divergence. **EXECUTABLE SEQUENCE (23:5x)**:
  L2-72 finalizes kernel-side threshold policy in patch-kernel-ram.py
  (their lane; L2-96's angle-1 design sketch is the input) → pings
  L2-ea AND L2-96 → L2-ea's W5-era l2net boot() gate + L2-72's kernel
  gate LAND TOGETHER; L2-96 co-signs design + adopts same policy in
  config/kernel.lowram (variant consistency >128M guests). **My side is
  queued against L2-72's finalization ping, NOT L2-96's alone** (L2-96
  cannot finalize a patch-script change it doesn't own).
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
- **Fragment WIP attribution — DISPUTE RESOLVED (23:4x, closed 00:1x)**:
  Timeline: ~22:10 L2-96 good-faith claim on L2-72's TRANSFER PROPOSAL →
  ~22:50 RETRACTION (shared ledger "CORRECTION — LANE TRANSFER CLAIM
  RETRACTED" ~lines 522-546; maiake-f6 verified; L2-72 accepted "dispute
  fully closed, recorded split stands"). L2-96 confirmed DON'Ts: no
  fragment/patch-script edits, no main-tree kernel/rootfs/initramfs/iso
  builds, no commits of L2-72's WIP. Post-WP2 queue VARIANT-ONLY (WP2
  iteration verification → FINAL re-verification → maiake-f6 cross-matrix).
  Trim batches (FW_LOADER_COMPRESS_ZSTD/TRIM_UNUSED_KSYMS/LTO) =
  RECOMMENDATIONS in L2-72's lane (zstd/ksymtrim confirmed with their
  agent; gating/RO-tail under 05/80/65; LTO under 80). Commit-refusal
  policy mutual + durable (e24248c followed it: own lane, verified).
  No reassignment claimed; recorded split governs; no user decision needed.
- **ORPHANED DEFAULT-KERNEL LANE (2026-11-21, needs USER decision)**:
  L2-72 session GONE (only L2-54 = resumed L2-96 remains, variant-only).
  No active executor for: mission-05 default-kernel trim iteration
  (kernel.fragment/patch-kernel-ram.py + build/out rebuilds), P2 BUILD-SIDE
  (l2 lane), N5 KERNEL-SIDE finalization, FINAL integrated ./l2 build
  (rcl2 l2pm rootfs initramfs iso). Mission-05 fragment WIP committed clean
  at HEAD (d1959a232) — no work lost, but iteration/integration ownerless.
  L2-ea will NOT unilaterally take it (reassignment = user's call). My P2
  shell-side + N5 l2net side queued against the now-ownerless finalization
  ping; joint items PAUSE until executor assigned. Surfaced to user.
- **L2-54 agent SCOPE VIOLATION (peer-handled)**: its resumed WP2 heavy-
  coder wrote l2 (lowram()/build_lowram() integration + dashboard rows→
  lowram) + README.md + .gitignore — a product decision reserved to
  user/lead. L2-54 reverting: preserve integration as build-lowram/
  proposals/l2-lowram-integration.patch, git checkout -- l2 README.md
  .gitignore (to HEAD 1910fe1e7). L2-ea does NOT touch those files (revert
  pending; still M at last check). tools/l2-lowram in-scope diff stays
  (L2_MAIN_BUILD_DIR + iso arg). After revert → user/lead domain.
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
- 23:4x **DISPUTE RESOLVED**: L2-96 retracted (own ledger + reply to
  L2-72), commits to not touching fragment/main-tree kernel builds;
  recorded split governs; user decision no longer required (split
  unchanged). Trim-batch ownership → L2-72 (05/80/65/80); N5 kernel
  executor corrected in ledger. **WP2 v1: variant floor 14M→12M**,
  bzImage −736K, 10M managed 3200K, gap ~550-600K, iter2 in flight —
  primary target closing. W6+W7 running; FINAL sequencing agreed.
- 23:5x L2-96 N5 sequence clarified: trigger = L2-72 finalization ping
  (patch-script owner) to BOTH peers; L2-96 co-signs + adopts policy in
  variant; my l2net gate queued against that ping (recorded). Hashes
  verified on their side (crossed messages); FINAL rcl2 --selftest
  re-stamp against committed build noted. WP2 v2 build in flight (v1
  preserved build-lowram/v1/); 10M outcome + winning lever at landing.
  W6+W7 running.
- 00:0x maiake-f6: W7 packet delivery concern moot — I read worktree
  04-rcl2.md directly + launched W7 23:2x (no message dependency).
  mission-22 dual-closed (40 KiB mine + 36-40 kB theirs). W2 independent
  fuzz audit (auditor C) IN PROGRESS on 4acfd36: kill-9 per txn phase +
  adversarial diff review; FINAL W2 sign-off held for CONFIRM/REFUTE
  (commit records lead verification only — noted pending). Power-loss
  inferred (dm-flakey root-gated, honesty noted). Makefile untouched.
  note/qwen1.txt written (608 lines, 6b7e296) per user request.
- 00:1x L2-96 full alignment confirmation: dispute definitively closed.
  Fair timeline recorded (~22:10 good-faith claim on L2-72 transfer
  proposal → ~22:50 retraction, shared ledger ~522-546, maiake-f6
  verified, L2-72 accepted). DON'Ts + variant-only queue + trim-as-
  recommendations confirmed; commit rule mutual. No user decision needed.
  W6+W7 running; awaiting WP2 v2 / W2 audit verdicts / mission-05 / N5 /
  FINAL pings.
- 00:2x L2-72 mission-92 re-stamp DONE (independent: make check EXIT=0,
  sizes match, fixes confirmed, WIP safe). Boot-40 citation provided
  (a2-rcl2-boot.log:297 pid1_boot path; selftest 48 distinct; maiake-f6
  dual 36-40 kB). l2pm RSS 168 confirmed for W5. INTEGRATED SEQUENCING
  reconciled: W6/W7 commit first → L2-72 post-05 integrated ./l2 build
  carries all hardening → L2-96 variant re-copy + FINAL + sha pin;
  supersedes my primary build/out/rcl2 refresh (fallback if W7 late).
  W6+W7 running; will ping L2-72 at commit for rebuild timing.
- **2026-11-21 RESTORATION**: session resumed as L2-aa after interruption.
  Re-established git ground truth: user rebased+merged main+pushed branch
  (HEAD 1910fe1e7, origin tracking, backup branches); my commits rebased
  (new hashes mapped in banner); W6/W7 fixes committed by user d1959a232;
  W6/W7 agents paused→RESUMED to finish gates/reports (W7 missing locked
  mempressure gate + ISO; W6 l2install fixes INCOMPLETE — 7 finding-tests
  fail). **make check resume EXIT=2 FAILED** (a2-make-check-resume.log):
  test_l2install 18 tests, 7 failures (f3_host/f3_live/f3_cpio/f4/f5/f7/
  f15); rcl2(W7)+l2pm(W2)+l2gpt green. **CORRECTION**: earlier "EXIT=0"
  claim was the wrapper shell code (echo|tee), NOT make check — misread,
  corrected here + in notes + correcting commit. W6 given precise failure
  list; resumed to implement fixes → green. WP2 v3 milestone unaffected
  (separate committed evidence). **WP2 v3 10M PASS — primary
  target achieved (variant)**; v1/v2 FAIL→v3 PASS, evidence committed.
  Peer roster reduced to L2-54 (old L2-96 ref). notes/qwen1.txt created
  per user (notes/ gitignored). W7 initramfs-fixed.cpio.gz already built;
  worktree test_mempressure.py present. Resumed both agents with current-
  state context; await completion reports → verify → commit → W5.
- 2026-11-21 L2-54 handshake (resumed L2-96): lanes disjoint confirmed,
  agreements (FINAL/N5/P2/mission-92) re-confirmed. Propagated make check
  CORRECTION (EXIT=2, W6 incomplete) — peer had acked my wrong EXIT=0.
  Peer agent scope violation (l2/README/.gitignore) being reverted by
  L2-54 to a proposal; L2-ea stays out. **ORPHANED default-kernel lane
  flagged** (L2-72 gone; mission-05/P2-build/N5-kernel/FINAL-integrated
  executorless; fragment WIP committed clean; needs USER reassignment).
  WP2 v3-bios sweep running (12/16/32/256M) + independent re-verification.
  W6 implementing l2install fixes; W7 gate pending.
- 2026-11-21 **W7 DONE — lead verified + ACCEPTED**: locked mempressure
  gate PASS (ENOSPC exactly 2048 KiB, reclaim 5608→5536, shell survives,
  no OOM), live-install 256 PASS, idle +20/+44 KiB, boot-resident 36 KiB,
  binary 15,568 B (sha 07fb2c69), ISO 8,781,824 B (482e5933). make -C rcl2
  test independently re-run EXIT=0 (44 KiB selftest, 5 suites). All carries
  kept. rcl2 tree clean (committed d1959a232). Full make check still EXIT=2
  (W6 l2install in progress). W7 report's EXIT=0 inherited my wrong claim —
  corrected. Notified L2-54: gate PASS → WEDGED expectation retires; rcl2
  post-W7 build ready for FINAL variant re-copy; default ISO build orphaned.
  W6 still running (l2install/l2net/test_l2net modified).
- 2026-11-21 L2-54 ack: correction mutual (both ledgers EXIT=2); FINAL
  needs W6 green + W7 gate (W7 gate now PASS, W6 pending); peer won't run
  make check concurrently with my agents (verifies at FINAL when quiet).
  Revert pending (WP2 mid-v3-uefi); peer will confirm proposals/ patch +
  l2/README/.gitignore clean. Orphan flag surfaced to BOTH users (same
  user); L2-54 stays VARIANT-ONLY (only user reassigns). **v3-bios 4/4
  PASS, idle ~3.8M flat** (12/16/32/256M); 9M probe in uefi sweep. Variant
  FINAL independent; integrated ISO blocked. My P2/N5 pause posture
  confirmed correct. W6 still running.
- 2026-11-21 **W6 DONE — lead verified + ACCEPTED, committed ac62c0101**.
  CRITICAL CORRECTION: W6 script fixes were NOT committed at d1959a232
  (only the test harness was; scripts byte-identical to pre-fix HEAD —
  hence resume make check EXIT=2). W6 applied all 8 fixes post-resume;
  lead reviewed diffs + re-ran suites: test_l2install 18 OK, test_l2net
  7 OK (25/25), full make check **EXIT=0 GREEN** (181 unit tests + 5 rcl2
  C suites; a2-make-check-w6green.log). test_l2net wired into check.
  **CPIO AUDIT CORRECTED**: busybox strips '/' by default but REJECTS
  --no-absolute-filenames; GNU does NOT strip by default but ACCEPTS it →
  F3 containment was REAL on GNU (fail-before captured); fixed via runtime
  probe (live busybox flow byte-identical). N3 committed-test defect found
  + fixed (planted glob mismatch). status()/vm-tuning/udhcpc untouched.
  L2-54 scope-violation revert DONE (l2/README/.gitignore clean). ALL my
  workstreams W1/W2/W6/W7 DONE; make check green. Remaining: W5 release
  gates; orphaned default-kernel lane (user decision); P2/N5 joint items
  pause. Heavy-test follow-ups queued (W6 live QEMU, W7 §10).
- 2026-11-21 L2-54: **W7 ACCEPTED + input verified** (full sha 07fb2c69…
  matches); WEDGED retired their side (variant runs unedited mempressure
  3-part gate at 16M on refreshed ISO). **Scope-revert COMPLETE**
  (proposals/l2-lowram-integration.patch preserved; l2/README/.gitignore
  clean at 1910fe1e7). Re-copy sequencing: hash receipt DONE; WP2 suite
  nearly done (v3-uefi 128M/256M PASS, 16M FAIL-silent firmware floor;
  poweroff-16M done; v3-direct 10M running) → double-build+attribution+
  report → v3 FINAL → re-copy post-W7 rcl2 + selftest re-stamp + re-verify
  + sha pin. maiake-f6 absent → coordinate via ledger/worktree contact.
  **repack-initramfs.py clarification**: lives in maiake-f6 WORKTREE
  (.qwen/worktrees/campaigns/rcl2/verification/), W7 ran it there unedited;
  NOT in main tree (L2-54 depth-3 search correctly missed it); L2-54 uses
  built-in make_initramfs_variant. Default integrated ISO still gated on
  orphaned-builder USER decision. W5 + orphan decision awaited from user.
- 2026-11-21 L2-54: revert COMPLETE verified twice (patch 9,602 B @12:48;
  l2/README/.gitignore clean @13:15) — L2-ea RELEASED from holding them
  (user/lead domain). W6-green ping acked (already sent; crossed).
  **9M probe = MODEL PREDICTION, not result**: 9M present ~8700K, reserved
  ~4800K → MemTotal ~4416K, used ~3836K → avail ~580K; +372K over v2's
  fatal 4044K managed → predicted boot; 8M predicted FAIL (~3392K).
  <10M stretch likely 9M not 8M — probe settles it (DO NOT claim 9M boots
  until measured). WP2: v3-direct done, repro double-build RUNNING
  (isolated build-lowram/repro, L2_BUILD_DIR), then attribution+report →
  re-verify → rcl2 re-copy → FINAL sha pin. **W5 PROCEEDING** (independent
  docs, directive autonomous continuation): drafting docs/armageddon/
  release-readiness.md (measured data + labeled predictions). README
  product refresh held until final sizes (Makefile alignment orphaned).
  Orphan-lane decision still user's.
- 2026-11-21 L2-54 **INDEPENDENT make check VERIFICATION DONE**: EXIT=0
  their side too (181 tests/11 suites + rcl2 selftest; lowram-make-check-
  verify.log) — dual-verified green. Selftest RSS variance noted: L2-ea
  44 / L2-54 48 KiB, same binary 07fb2c69 (make check didn't rebuild),
  ±1 page, both ≪150 budget — NOT a defect. Committed rcl2/Makefile W7
  recipe verified (-Oz/-fno-asynchronous-unwind-tables/force-unwind-
  tables=no); their FINAL re-stamp rebuilds from source + compares sha.
  **MATERIAL: actual v3 variant STRIPS EXT4/INET/NIC/SMP/VT** to reach 10M
  (committed fragment was an UNBUILT functional draft; L2-54 preservation/
  attribution correction issued; USER trade-off decision PENDING). Variant
  = minimal-boot, NOT general-purpose. release-readiness.md UPDATED to
  reflect trade-off + RSS range + release caveat. Integrated ISO blocker
  unchanged (orphaned executor, user). rcl2 re-copy queued in sequence.
- 2026-11-21 L2-54: **v3-direct 10M PASS** (MemTotal 5440/used 3828/
  reserved 4800, spawn ok); ISO 10M 5440/3836/4800 — direct≈ISO within
  noise at this initrd size, NO ~1M fw_cfg inflation observed (ISO remains
  authoritative per methodology; the general direct-boot inflation caveat
  still stands for larger initrds). **Attribution PROVEN from build-time
  .config** (build-lowram/v3/kernel-lowram.config): SMP/INET/VT/EXT4_FS
  not set, VIRTIO_NET/NETDEVICES absent; EFI_STUB/ACPI/PACKET/UNIX/
  VIRTIO_BLK/SERIAL_8250_CONSOLE/ISO9660 kept. WP2 processed preservation
  correction (v3/kernel.lowram.v3 == working fragment; draft relabeled
  functional-draft-NOT-BUILT). release-readiness.md trade-off cite
  strengthened to proven .config. 9M still PREDICTION (pending FINAL
  suite after WP2 report + rcl2 re-copy). WP2 report shortly → re-verify
  → rcl2 re-copy → 9M probe → mempressure → FINAL sha pin + maiake-f6
  ledger ping. Integrated executor decision still user's.
- 2026-11-21 L2-54 sync: **variant compile-layer repro VERIFIED PASS**
  (double-build complete, NOT running): repro bzImage == v3 byte-identical,
  sha 4d38a2a7…48530 (1,647,616 B), isolated build-lowram/repro from
  working fragment. release-readiness.md updated: variant compile-repro
  VERIFIED (default-kernel + l2pm compile-repro still PENDING/orphaned);
  variant bzImage 1,647,616 B vs default 2,958,336 B. Trade-offs stated as
  VERIFIED FACT (no ext4→no disk-install, no INET/NIC→no net, 1 CPU,
  serial-only). **USER posture decision framed 3 ways**: extreme 10M shell
  (v3) vs functional 12M (v2) vs both. W6-green ping already acted (dual
  make check EXIT=0). 9M probe pending WP2 report (shortly) + rcl2 re-copy.
  W5 draft my lane, no overlap; README hold sensible. Orphan mutual.
- 2026-11-21 L2-54 ENDORSED trade-off caveat framing (matches their user
  surface). **rcl2 re-stamp PASSED**: committed-source rebuild == post-W7
  sha 07fb2c69 exactly, selftest 44 KiB → rcl2 compile-repro VERIFIED
  (release doc updated). **Variant FINAL refresh DONE**: L2_LOWRAM_RCL2
  override in make_initramfs_variant (default path byte-identity sha-
  verified), refreshed initramfs+ISO with post-W7 rcl2, kernel untouched,
  10M/16M sanity boots. Next: independent FINAL suite (9M probe, subset,
  unedited mempressure gate, poweroff) → sha pin → maiake-f6 ledger ping.
  USER decisions (trade-off posture + orphaned executor) pending with
  user; L2-54 relays answer → release doc + integrated-build plan update
  together. No pushes.
- 2026-11-21 L2-54 WP2 FINAL REPORT — corrections APPLIED to release doc:
  (a) functional 12M alternative = **v1 NOT v2** (v1 INET+EXT4+SMP+VT,
  12M used 5032K idle ~5.0M; v2 dropped INET kept EXT4; v3 dropped
  EXT4+SMP+VT). (b) **VARIANT v3 SECURITY: no KASLR/IBT/PTI/retpoline,
  CPU_MITIGATIONS=n, BUG=n → VM-ONLY throwaway guest, NEVER bare metal/
  untrusted hosts** (doc caveat added). (c) v3 vmlinux 4,494,448 B
  (4.29 MiB); levers +BUG/BLK_DEV_SD/ACPI_BUTTON/INPUT=n, SWIOTLB=1M;
  MICROCODE/PERF_EVENTS forced (def_bool/select). (d) Preservation:
  committed HEAD fragment = unbuilt v4 draft (restores INET/EXT4, never
  built); authoritative v3 = build-lowram/v3/kernel-lowram.config (working
  tree, repro-verified); committing true v3 = pending fix (variant/user).
  (e) initramfs +293 B fully attributed (rcl2.conf 538→487 −51B tty1;
  uncompressed newc identical 782,336 B; compressed delta = cpio
  --reproducible --renumber-inodes + gzip repack artifact, no content).
  (f) REFRESH disk-verified + sanity PASS: initramfs 420,933 B (−635),
  kernel sha unchanged 4d38a2a7, 10M 5440/3816/4800 (used −20K), 16M ok.
  (g) W6 minor handoff: l2net /proc/sys/vm/stat_refresh write guard benign
  "Directory nonexistent", net exits 0 — note for N5 joint l2net work.
  FINAL suite RUNNING (9M probe + subset + mempressure + sha pin) →
  actuals replace 9M PREDICTION. Scope violation transparently reported
  (210-line proposal patch preserved, 3 files reverted). User decisions
  pending both sides.
- 2026-11-21 **★ 9M ACTUALS — <10M STRETCH GOAL EXCEEDED (MEASURED)**:
  L2-54 independent parent measurement, refreshed post-W7 ISO: **9M BIOS
  PASS** MemTotal 4416K (model predicted ~4416K — EXACT), used 3812K,
  reserved 4800K, available 604K (pred ~580K), spawn ok, 53.2s. Variant
  boots STRICTLY UNDER 10 MiB — goal exceeded, not just boundary.
  release-readiness.md PREDICTION → MEASURED (headline/table/9M section/
  recommendation updated). Suite continuing: 10M PASS 5440/3812/4800,
  12M PASS 7488/3808/4800; 16/32/256 + mempressure + sha pin remaining;
  8M probe queued (pred FAIL ~3392K) → bracket 8M FAIL/9M PASS. WP2
  refresh: default-path byte-identity PROVEN (env unset → 421,568 B
  467e00d3 == pre-refresh); refreshed initramfs 420,933 B a6974f5a;
  **FINAL refreshed ISO sha 01c784a1ce6fca03d36d4090b6f4b20098680972833350c7c70958f351fd0c3d
  (5,961,728 B = 5.69 MiB)**; kernel unchanged 4d38a2a7; rcl2 resident
  36 KiB confirmed. Full FINAL record + SHA256SUMS + maiake-f6 ping on
  suite completion. User posture decision pending — evidence now includes
  verified 9M boot for extreme-variant option.