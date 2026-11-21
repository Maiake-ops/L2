# PROJECT ARMAGEDDON — L2 ecosystem engineering ledger

Canonical progress record for the 100-mission campaign (started 2026-11-20).
Supersedes the earlier 12-campaign `PROGRESS.md` (kept for history; its baseline
logs live in `build/campaigns/`). Resume protocol: read this file, run
`git status`, continue the first mission not marked DONE.

## Ground rules
- Uncommitted `rcl2/` changes (Makefile, src/main.c, src/rs/rcl.rs) are
  user-owned WIP memory optimizations: PRESERVE, verify, never revert.
- No destructive ops, no remote changes, no git history rewrites without
  explicit approval. Small reviewable diffs only.
- Every claim needs a command + output. Logs under `build/campaigns/`,
  mission reports under `docs/armageddon/`.
- Budgets (user-confirmed): core ISO < 10 MiB hard gate; installed storage
  < 10 MiB; idle RAM 10–15 MiB band (hard ceiling 15 MiB = 15360 KiB) at any
  guest RAM size. Low-RAM boot reliability (10–32 MiB guests) is a campaign
  goal, distinct from the idle-RAM band on big machines.
- RAM measurements: boot the real ISO (`tools/ramtest.py --iso`), `-smp 2`,
  settle 40 s; direct `-kernel/-initrd` reads ~1 MiB high (fw_cfg).

## Team routing
- Implementation: **heavy-coder** (qwen4-max). Definition restored to
  `.qwen/agents/heavy-coder.md` on 2026-11-20 (was parked as `.md.save` with
  truncated frontmatter; `.save` left untouched).
- Analysis/audit missions: main agent (read-only) or Explore.
- Test campaigns: run via project tools (`make check`, ramtest, stability);
  flash-tier agents are not registered in this session — tier fallback noted.

## Baseline evidence (2026-11-20, this host: 16 cores, KVM, ~13 GiB RAM)
- `make check`: PASS, exit 0 — `build/campaigns/baseline-make-check.log`
  (17+45+19+3+4+17+10+4 unit tests OK; rcl2 selftest 48 KiB resident).
- Artifacts (14:39 build): l2.iso 8,787,968 B (8.38 MiB, under gate);
  vmlinuz-l2 2,958,336 B; initramfs-l2 426,833 B; rcl2 26,904 B;
  busybox 374,752 B; dash 145,168 B; l2pm 88,184 B.
- Prior RAM sweep (`build/campaigns/baseline-ram-sweep.log`, truncated):
  **FAIL 10M** (no prompt), **FAIL 12M** (early-boot OOM panic:
  "System is deadlocked on memory"; DMA32 present 11768 KiB, managed only
  3212 KiB → ~8.5 MiB reserved before init). Log cuts off at 16M.
- Fresh characterization sweep (ISO copy `build/campaigns/baseline-l2.iso`,
  sizes 10M…1G, settle 40, budget 15360): COMPLETE,
  `build/campaigns/armageddon-baseline-sweep.log`:
  10M FAIL (no prompt); 12M FAIL (early OOM panic, managed 3212K of 11768K);
  16M 5564K/8192K; 24M 5632K/16256K; 32M 5572K/24320K; 64M 5556K/56576K;
  128M 5564K/121092K; 256M 5548K/250120K; 1G 5584K/1024264K — all PASS.
  Idle RAM flat ~5.5 MiB ≪ 15 MiB ceiling. Boot floor is between 12 and 16 MiB.
- Early-boot forensics (32M, direct kernel, memblock=debug+initcall_debug):
  `build/campaigns/boot-forensics-32M.log`. Memory: 22252K/32248K available,
  9060K reserved; image = 4891K code + 351K rwdata + 1392K rodata + 692K init
  + 164K bss. vmlinux: text 5626568 + data 1928616 + bss 299008.
- OVMF present: ~/.nix-profile/share/qemu/edk2-x86_64-code.fd (UEFI tests possible).

## Mission ledger
Status: [ ] pending · [~] in progress · [x] done (evidence linked) · [!] blocked

### Campaign A — kernel engineering
- [x] 01 Analyze complete x86_64 boot process → docs/armageddon/01-boot-process.md
- [x] 02 Audit entire current kernel configuration → docs/armageddon/02-04-kernel-config-memory.md + build/campaigns/02-config-audit-symbols.txt
- [x] 03 Profile kernel memory allocation during early boot → same report (earlycon re-capture pending under 16)
- [x] 04 Investigate physical memory reservation → same report; floor ≈8–8.5 MiB, image-dominated
- [~] 05 Optimize kernel image size → OUR LANE (peer reserved kernel.fragment/patch-script to us; their lowram variant is additive-only). heavy-coder RESUMED: trim unrequested built-ins ≥500 KiB, shrink hdr.init_size (synergy with peer's 10M triple-fault root cause), keep real-HW support; contention-safe JOBS=6 L2_LOAD=5; evidence build/campaigns/05-kernel-size.md
- [~] 06 Optimize kernel decompression → maiake-f6 ANALYSIS lane (worktree campaigns/engineering, additive config/kernel.slubdebug + tools/l2-slubdebug.sh); we verify/integrate
- [~] 07 Analyze page-table memory overhead → same maiake-f6 lane
- [~] 08 Minimize slab allocator overhead → same maiake-f6 lane (input: L2-96 16M Slab 3376K dominant)
- [~] 09 Investigate init memory reclamation → same maiake-f6 lane (streaming initrd free already verified under mission 13)
- [~] 10 Validate low-memory kernel boot reliability → L2-96: failure-side forensics done (10M BIOS silent triple fault = decompressor/initrd collision; 10M direct kthreadd OOM managed 1152K; 12M OOM managed 3212K; 16M accounting slab-dominated) — build/campaigns/lowram-forensics-report.md; full BIOS+UEFI matrix running (build/campaigns/lowram-default-matrix/); lowram-variant heavy-coder in flight (disjoint lane: config/kernel.lowram + tools/l2-lowram + build-lowram/)

### Campaign B — boot infrastructure
- [x] 11 Audit BIOS bootloader implementation → docs/armageddon/11-13-boot-infrastructure.md
- [x] 12 Audit UEFI bootloader implementation → same report; UEFI 256M PASS idle 5760K (baseline-uefi-forensics.log)
- [x] 13 Analyze initramfs memory placement → same report; streaming reclaim verified; NO swiotlb reserve on small guests
- [~] 14 Implement reproducible initramfs generation → maiake-f6 lane (worktree build_initramfs verification); mechanism exists (sorted newc + gzip -n + epoch); ISO assembly byte-repro verified by them (see 52)
- [~] 15 Validate compression format compatibility → maiake-f6 lane (negative tests); gzip-only by design, RD_* others=n
- [ ] 16 Develop boot-stage instrumentation (earlycon+memblock=debug recipe proven; initcall_debug works; consider permanent tool)
- [~] 17 Investigate KASLR-related boot failures → L2-96 + maiake-f6 agent A (independent TCG proof): 10M ISO triple fault (#GP IP=0x9a1c90 in top-of-RAM initrd region) is a DETERMINISTIC arithmetic collision — PHYSICAL_START 0x200000 + init_size 0x7e4000 = 0x9e4000 > usable top 0x9e0000 — NOT KASLR-caused (earlier "KASLR adds nondeterminism" framing refined; RANDOMIZE_BASE=y confirmed ON, variant sets n for determinism margin). PHYSICAL_START cannot go below 0x200000 on x86_64 (PHYSICAL_ALIGN range, arch/x86/Kconfig:2095-2099, verified) → span shrink is the ONLY placement lever; "lower PHYSICAL_START" lever retracted — build/campaigns/lowram-forensics-report.md + build/lowram-forensics/10M-tcg-trace.log
- [ ] 18 Validate memory-map edge cases
- [~] 19 Test boot behavior under memory pressure → L2-96 CORRECTION: 14M boots interactive (MemTotal 6144K, avail ~492K), 30 sequential spawns OK, but 2×8-way parallel burst OOM-kills (all 16 jobs die; PID1+shell survive, avail recovers 580K) — zero parallel headroom at 14M (earlier "burst OK" was a marker-only false positive; log shows OOM at kts 43.14). 13M OOM-deadlock → BIOS floor 13M FAIL / 14M PASS; floor model installed_min ≈ reserved(8192K)+used(5536K)+headroom; **16M comparison: identical workload CLEAN (no OOM, 0 killed, avail 2536→2432K)** — usable parallel headroom starts at 16M — build/lowram-forensics/14M-bios-stress.log + 16M-bios-stress.log + lowram-forensics-report.md
- [x] 20 Automate QEMU boot regression testing → tools/lowram_matrix.py + tools/test_lowram_matrix.py (**39** unit tests OK incl. ANSI-stripping + limine-panic signature from real 64M UEFI capture; **committed e24248c** by L2-96 — correction: 1888794 carried the patchset + other tests, NOT this harness); default-ISO matrix COMPLETE: build/campaigns/lowram-default-matrix/ (L2-96 lane)

### Campaign C — rcl2 init
NOTE: implementation lane owned by peer L2-ea (ARMAGEDDON II); we audit/verify/hand off.
- [x] 21 Audit PID1 process lifecycle → docs/armageddon/21-rcl2-lifecycle.md (gap map for 23–30; CGROUPS=n ⇒ 28 must use RLIMITs)
- [~] 22 Optimize baseline PID1 memory usage → user WIP (cd0f88a) VERIFIED: binary 26,904→17,184 B (−36%); selftest resident 48 KiB (budget 150) unchanged (static buffers dominate RSS); boot-resident re-measure pending; addendum in 21-rcl2-lifecycle.md
- [ ] 23 Implement service dependency resolution
- [ ] 24 Implement parallel service startup
- [ ] 25 Implement crash recovery
- [ ] 26 Implement watchdog supervision
- [ ] 27 Implement service health monitoring
- [ ] 28 Implement resource controls
- [ ] 29 Implement emergency recovery behavior
- [ ] 30 Run lifecycle and fault-injection tests

### Campaign D — userspace
- [x] 31 Audit the minimal shell → docs/armageddon/31-33-userspace-shell-libc.md (dash 0.5.12 static musl -Os strip -s, 145,168 B; FLAG: source URL plain HTTP → missions 53/69)
- [~] 32 Investigate libc memory overhead → audit done (static musl 1.2.5, no libc in rcl2; idle duplication bounded at ≤5 processes); per-process RSS measurements pending (40/72)
- [~] 33 Evaluate musl, glibc, and static linking → decision documented: KEEP STATIC musl (glibc incompatible with budgets; shared musl a wash at this process count); input for mission 91
- [ ] 34 Optimize essential userland utilities
- [ ] 35 Implement minimal process inspection
- [ ] 36 Implement minimal networking utilities
- [ ] 37 Investigate device management
- [ ] 38 Test filesystem mounting and recovery
- [ ] 39 Minimize binary and runtime dependencies
- [ ] 40 Benchmark minimal interactive userspace

### Campaign E — package management
NOTE: implementation lane owned by peer L2-ea. AUDIT COMPLETE (Explore + lead verification) → build/campaigns/armageddon-l2pm-gapmap.md (handed off 20:50; l2pm.c mtime 08:37, line numbers valid then).
- [x] 41 Design L2PM package metadata → implemented-basic (recipe keys + DB + per-file manifests + .l2pm/info; no desc/license/arch/provides; deps bare names) — gapmap §41
- [x] 42 Implement package installation → implemented (hash-first, mkdtemp, hardened extract, double-rename atomic, store never overwritten); durability gap → 44 — gapmap §42
- [~] 43 Implement dependency resolution → RESOLVED for `add` (recursive, cycle detection, depth 32, reverse-dep remove guard); `install` only warns; no version solving; last-wins file conflicts — impl L2-ea — gapmap §43
- [~] 44 Implement transactional upgrades → partial: per-package atomic + generation symlink switch; multi-package partial failure leaves half-upgraded set; zero fsync — impl L2-ea — gapmap §44
- [~] 45 Implement rollback → implemented but FRAGILE: VERIFIED default gc deletes inactive generations (C:2586–2588); rollback checks is_dir not hashes — impl L2-ea (top gap #1) — gapmap §45
- [ ] 46 Implement package signatures → MISSING (VERIFIED grep 0 sig/fsync hits; TOFU gate covers recipe exec only; install has no authenticity check) — impl L2-ea (top gap #4) — gapmap §46
- [ ] 47 Implement repository metadata → MISSING (bare .recipe files; no index/signing/mirrors) — impl L2-ea — gapmap §47
- [~] 48 Implement package caching → partial (tarball/git/recipe caches + offline reuse; basename-keyed, no invalidation/re-fetch, cached .l2p never reused, gc never cleans cache/tmp) — impl L2-ea (top gap #5) — gapmap §48
- [x] 49 Implement source package builds → implemented (4-step pipeline, 40-hex ref pin + injection guard, epoch mtimes, deterministic ustar, --check double-build); undocumented recipe-epoch precedence — gapmap §49
- [~] 50 Test package database corruption recovery → detect-only (verify catches tamper; VERIFIED missing-manifest passes as legacy C:2258; corrupt DB ⇒ gc deletes live store; no fsck/repair) — impl L2-ea (top gaps #2/#3) — gapmap §50

### Campaign F — build infrastructure
- [~] 51 Audit all existing build scripts → docs/armageddon/51-build-scripts-audit.md (l2 orchestrator + Makefiles DONE: repro hygiene good, kernel byte-identical verified; findings: dash HTTP resolver+empty-sha update pin, empty-pin TOFU acceptance, curl -L redirect downgrade, no download max-time, non-atomic lock update, limine resolver/source host mismatch; fixes DEFERRED until no peer ./l2 build runs; l2install destructive-path + overlay + mkimage.sh deep review PENDING)
- [~] 52 Implement reproducible builds → maiake-f6 VERIFIED (worktree build/campaigns/07-reproducible.md): ISO assembly byte-reproducible (2 staging rebuilds == baseline 06a4fd1e…); l2pm cross-checkout-path build byte-identical (efc32819…, musl wrapper abs paths don't leak); kernel byte-identical (L2-96 rebuild + our hash checks); heavy-coder double-ISO spot-check under 05; full double-build-all still pending
- [~] 53 Implement source integrity verification → maiake-f6 worktree lane + OUR main-tree queued fixes (dash https, refuse empty-sha http pin, --proto-redir =https, download max-time, atomic lock update — deferred until no ./l2 builds run); their worktree Makefile musl-prerequisite fix (finding g) to port to main tree (ours, cite their diff)
- [ ] 54 Implement cross-compilation support
- [ ] 55 Implement dependency-aware build scheduling
- [ ] 56 Implement incremental build caching
- [ ] 57 Implement automated compiler diagnostics
- [ ] 58 Implement artifact integrity checks
- [ ] 59 Implement one-command ISO creation
- [ ] 60 Build an automated release verification pipeline

### Campaign G — security
- [~] 61 Audit kernel attack-surface assumptions → docs/armageddon/61-69-security-posture.md §61 (no LSM/FORTIFY/HARDENED_USERCOPY/SECCOMP/STACKPROTECTOR/MODULE_SIG; KASLR/RWX/IBT/stack-zero on; DEVMEM/PROC_KCORE off; SMAP/SMEP/PTI symbols absent in 7.3.3 — runtime verify pending; toggles are budget trade-offs, decisions → campaign J)
- [~] 62 Audit PID1 privilege handling → same report §62 (root console by design, no auth; config root-owned trust boundary; busybox login/passwd/su/getty present but bypassed; no shadow — login/su behavior untested → 67)
- [x] 63 Audit package signature validation → same report §63 + gapmap §46 (none; TOFU gate = recipe exec only; impl L2-ea design-only)
- [x] 64 Audit filesystem permissions → same report §64: no setuid/setgid, no ww-dirs w/o sticky, passwd/group 0644, no shadow (by design →67), /dev empty in staging (rcl2 mounts devtmpfs; cpio owner 0:0 normalizes) ✔; hardening: /root 755→700 (our deferred batch), /tmp 755→1777+size cap (interacts maiake-f6 finding (d) → L2-ea rcl2 lane); store immutability by convention (verify detects); OPEN: host `l2install dir` uid preservation (51 pending review)
- [ ] 65 Investigate unsafe memory operations
- [x] 66 Audit subprocess execution → same report §66 COMPLETE: l2pm sh -e -u -c fixed env + injection guards; l2net F1+N3; l2install via L2-ea W6 audit (F3/F4/F5/F7/F14, cite their §W6); udhcpc.script CLEAN (L2-ea audit + lead spot-check VERIFIED); shutdown signals-only
- [ ] 67 Implement malformed-input testing → scope split: W1 (L2-ea) owns rcl2 Rust-parser fuzz + truncation regression (ABI limits relayed; corpus share pending); OURS = busybox login/su missing-shadow behavior + non-rcl2 malformed input (l2pm tar/recipe edges: their W2 + existing 45 security tests)
- [ ] 68 Investigate race conditions
- [~] 69 Evaluate reproducible-build supply-chain security → same report §69 (F1 VERIFIED: l2net find_pack no L2-LIVE check → ring-0 modules from untrusted media, fix L2-ea W6; dash http:// hardcoded l2:293/327/683, l2 update empty-sha pin over HTTP → our build-infra fix deferred; kernel repro byte-identical verified)
- [ ] 70 Execute regression tests for confirmed vulnerabilities (F1 regression test after W6 fix)

### Campaign H — performance
- [ ] 71 Measure cold boot latency
- [ ] 72 Measure steady-state memory usage
- [ ] 73 Measure process creation overhead
- [ ] 74 Measure filesystem overhead
- [ ] 75 Measure shell startup overhead
- [ ] 76 Measure service startup latency
- [ ] 77 Measure package installation performance
- [ ] 78 Measure build-system performance
- [ ] 79 Identify top five performance bottlenecks
- [ ] 80 Implement and validate targeted optimizations

### Campaign I — compatibility
- [x] 81 Test guest RAM sizes 10 MiB–256 MiB → L2-96 matrix COMPLETE (20 runs, snapshot of rebuilt default ISO, WIP rcl2, settle 40, smp 2): BIOS 14–256M PASS (8), 10M silent-triple-fault / 12M oom-deadlock FAIL; UEFI 128/256M PASS, ≤64M FAIL; idle used flat ~5.5M BIOS / ~5.7M UEFI; results.json + summary.md + full serial logs: build/campaigns/lowram-default-matrix/
- [ ] 82 Test multiple QEMU CPU models
- [x] 83 Test BIOS configurations → same matrix (Limine hybrid ISO, SeaBIOS): floor bracketed 13M FAIL / 14M PASS (14M interactive but zero parallel headroom, 16M clean under burst); 10M silent triple fault (decompressor/initrd collision, mission 17), 12M oom-deadlock @0.40s
- [x] 84 Test available UEFI configurations → same matrix (OVMF edk2-x86_64-code.fd): **UEFI floor ∈ (64M,128M]** — 10–32M silent (OVMF stage), 64M Limine PANIC "High memory allocator: Out of memory" in init_memmap (ANSI-decoded; harness now classifies limine-panic), 128M PASS used 5744K reserved 15900K, 256M PASS used 5712K; UEFI costs ~5.9M more reserved than BIOS; sub-10M is BIOS-only (firmware constraint, demonstrated) — maiake-f6 REFINEMENT (worktree evidence): 32/48/64/96M FAIL; 48/64M panic inside Limine uefi_entry→init_memmap→ext_mem_alloc_type_aligned_mode BEFORE kernel load; 128/256M PASS idle ~5728/5748K (~160K above BIOS); floor ∈ (96M,128M]; sub-10M objective BIOS-only
- [ ] 85 Test different initramfs compression modes
- [ ] 86 Test filesystem compatibility
- [ ] 87 Test kernel command-line variations
- [x] 88 Test clean shutdown and reboot → L2-96 at 256M BIOS: poweroff → clean rcl2 shutdown + QEMU exit 0; reboot → QEMU exit 0 (-no-reboot); ALSO S3 suspend/resume via RTC wakealarm verified clean (PM suspend entry→ACPI low-level resume complete→suspend exit; spawn after resume OK) — downgrades the patchset low-1M-reservation flag to real-HW residual risk — build/lowram-forensics/suspend-A.log + reboot-B.log
- [~] 89 Test fault recovery → L2-96: test-stability.py 16M/32M PASS incl. console_service_restart (rcl2 respawn recovery under pressure), invalid_config_parser, orphan reaping, ext4 clean-reboot/recovery — build/campaigns/lowram-stability-default/report.json; deeper fault injection stays with L2-ea rcl2 lane (25/30); note maiake-f6's /tmp uncapped-ramfs defect also lands here
- [ ] 90 Create documented compatibility matrix

### Campaign J — final engineering review
- [ ] 91 Review every architectural assumption
- [ ] 92 Reproduce all claimed improvements
- [ ] 93 Search for hidden regressions
- [ ] 94 Review all benchmark methodologies
- [ ] 95 Audit final build artifacts
- [ ] 96 Validate final memory budget
- [ ] 97 Validate final boot reliability
- [ ] 98 Produce complete technical documentation
- [ ] 99 Create prioritized remaining-defects backlog
- [ ] 100 Deliver verified release candidate if all gates pass → agreed gate battery (maiake-f6 release-matrix harness + our verification): ISO <10 MiB hard; idle ≤15360 KiB (user-confirmed 10–15 MiB band 2026-11-13; 10240 KiB = old ramtest default, stretch only — correction sent to maiake-f6); BIOS floor per mission-05 trim outcome; UEFI ≥128M documented constraint (sub-10M BIOS-only); make check green; repro double-build (kernel+ISO byte-identical); live-install + installed-boot test; F1/P2 regression after W6

## Concurrency (IMPORTANT — read before editing)
- A PARALLEL session is active in this repo (observed 20:15): building the
  kernel in git worktree `.qwen/worktrees/campaigns` (branch
  `campaigns/engineering`, -j8 -l6), testing against
  `build/campaigns/baseline-artifacts/` (their copies, with SHA256SUMS), and
  authoring untracked `tools/lowram_matrix.py` + `tools/test_lowram_matrix.py`
  in the main tree (low-RAM BIOS/UEFI matrix with failure classification).
- User committed the rcl2 WIP as `cd0f88a` ("qwen burning credit time",
  20:04) and switched main-tree HEAD to branch `campaigns/armageddon-ii`;
  `campaigns/engineering` holds the old main (21031ce). PROGRESS.md deleted.
- Our lane: heavy-coder edits ONLY config/kernel.fragment (+ justified
  patch-kernel-ram.py hunks) and builds in the MAIN tree; campaign lead owns
  docs/armageddon*. DO NOT touch: tools/lowram_matrix*.py,
  .qwen/worktrees/**, build/campaigns/baseline-artifacts/**, branch state,
  rcl2/** (now committed user work). Kernel-config edits in the two trees
  may need merging later — flag, don't silently resolve.
- UPDATE (20:30, lane split CONFIRMED via peer's ledger entries): L2-96
  rebuild COMPLETE (build/out 20:06); vmlinuz + kernel.config byte-identical
  to baseline → kernel reproducibility CONFIRMED. ISO hash delta explained:
  committed rcl2 WIP (17,184 B) → initramfs 421,275 B — not a repro failure.
  Peer is ADDITIVE-ONLY (config/kernel.lowram, tools/l2-lowram, build-lowram/
  — off-limits to us) and RESERVES mission 05 (kernel.fragment +
  patch-kernel-ram.py) to us. Our heavy-coder RESUMED (stand-down superseded)
  with JOBS=6 L2_LOAD=5 and baseline-artifacts/ as canonical comparison.
  Peer's 10M root cause: decompressor/initrd collision (PHYSICAL_START
  0x200000 + init_size 0x7e4000 > usable top 0x9e0000) — our image trim
  shrinks init_size (direct synergy). Canonical original snapshot:
  build/campaigns/baseline-artifacts/ (SHA256SUMS verified == our
  baseline-l2.iso 06a4fd1e…). Our baseline-initramfs copy is post-rebuild
  (rcl2 WIP); baseline-vmlinuz/kernel.config match the snapshot exactly.
- THIRD SESSION (20:40): **L2-ea** ("ARMAGEDDON II directive", same user)
  claims write scope: rcl2/, l2pm/, tools/test_rcl2*, docs/armageddon-ii-progress.md,
  build/a2-*/, build/campaigns/a2-*. It created branch campaigns/armageddon-ii
  from cd0f88a for its rcl2/l2pm workstreams. It misattributed lowram_matrix.py
  to us — corrected (it's L2-96's). LANE ACCEPTED: we cede rcl2/l2pm
  IMPLEMENTATION (missions 23–30, 41–50) to L2-ea; we keep audits/verification
  and hand off gap maps (Explore l2pm map → build/campaigns/armageddon-l2pm-gapmap.md).
  We warned L2-ea that our heavy-coder rebuilds build/out/{vmlinuz,initramfs,ISOs}
  (mission 05, reserved lane) — peers must test against snapshots. heavy-coder
  told: if make check fails in rcl2/l2pm stages, check peer WIP (git status)
  before attributing; never edit those sources.
- THREE-SESSION LANE MAP: L2-96 = additive lowram variant (config/kernel.lowram,
  tools/l2-lowram, build-lowram/) + lowram_matrix tooling + low-RAM boot goal;
  L2-ea = rcl2/l2pm implementation + a2 logs/docs; US = default-kernel trim
  (config/kernel.fragment, tools/patch-kernel-ram.py — mission 05, reserved by
  L2-96), kernel missions 06–09, boot-infra/userspace/build-infra/security/
  perf/compat audits+tests, docs/armageddon*, final review. NO commits by us;
  all peers informed. Mutual pings before cross-lane writes or build/out rebuilds.
- CONFIRMED (21:05, L2-ea reply): split STANDS — mission 05 stays ours;
  L2-ea's directive is ADDITIVE (missions 101–300), they coordinate
  workstreams and own verification/commits on campaigns/armageddon-ii
  (commits by them only, after verification). Exact write scope: rcl2/,
  l2pm/, tools/test_rcl2_lifecycle.py + tools/test_l2pm_transactions.py
  (new), extending tools/test_l2pm_*.py, docs/armageddon-ii-progress.md,
  build/a2-*/, build/campaigns/a2-*; PLUS W6 shell-safety hardening of
  l2install/l2net ⇒ implementation of those files is THEIR lane (our
  audits stand). Their agents never write build/out, never run ./l2 build;
  W1 boot-tests against the canonical snapshot; ping before live reads.
  CORRECTION: cd0f88a was committed on MAIN; L2-ea branched
  campaigns/armageddon-ii from it afterwards. W1 will capture boot
  "resident N KiB" → closes mission-22 re-measure. W2 (l2pm transactions/
  torn-state/solver) mid-flight; signatures/binary-repos design-only;
  cross-validating against our gapmap. Their l2net find_pack finding
  VERIFIED + recorded as F1 (docs/armageddon/61-69-security-posture.md);
  fix their W6, regression test our mission 70.
- FOURTH SESSION (21:40): **maiake-f6** (cwd /home/maiake; the worktree
  builder observed at 20:15). Works ONLY in .qwen/worktrees/campaigns
  (branch campaigns/engineering, base 21031ce), never writes main checkout;
  read our ledger and REDIRECTED its kernel/rcl2/l2pm implementation agents
  to verification/analysis/handoff (no duplication of mission 05 / L2-ea).
  LANES (worktree, additive): missions 06–09 analysis (config/kernel.slubdebug
  + tools/l2-slubdebug.sh); 14–15 initramfs repro verification + compression
  negative tests (build_initramfs in l2 only); 51–53 build-infra audit/repro/
  integrity (top-level Makefile musl-prerequisite fix landed in worktree);
  l2wifi security fuzz (l2wifi/, tools/test_l2wifi*.py); release-matrix
  harness tools/test-release-matrix.py (missions 60/97/100 gate battery:
  BIOS+UEFI sweeps, guest compat, mempressure, live-install, make check,
  l2pm repro, artifact gate); guest-compat battery tools/test_guest_compat.py
  (82/86–90) + overlay/bin/l2net status exit-code fix.
  VERIFIED FINDINGS (cite; evidence worktree build/campaigns/): (a) ISO
  assembly byte-reproducible == baseline 06a4fd1e (07-reproducible.md);
  (b) l2pm cross-checkout-path byte-identical efc32819; (c) UEFI floor
  ∈ (96M,128M] — 32/48/64/96M FAIL, Limine uefi_entry→init_memmap→
  ext_mem_alloc panic BEFORE kernel load; 128/256M PASS ~5728/5748K;
  sub-10M BIOS-only; (d) NEW rcl2 defect: live-mode /tmp uncapped ramfs →
  OOM/respawn wedge (09-oom.md; routed to L2-ea — missions 19/29/93);
  (e) l2net status exited 1 without resolv.conf — fixed in worktree
  (if-form, proven with shipped dash); (f) l2install image mode needs
  truncate+losetup (util-linux) absent in guest busybox → host-only in
  practice, guard recommended (for L2-ea W6); (g) fresh-clone make check
  fails on NixOS without musl wrapper — fixed via order-only prerequisite
  (worktree Makefile; main-tree port queued OURS).
  OVERLAP FLAGS: (1) their worktree l2net status fix vs L2-ea W6 main-tree
  l2net hardening — coordinate at merge (user reviews branches);
  (2) mission 51 dual audit (our main-tree report + their worktree audit)
  — complementary, cite both; (3) main-tree l2 edits (dash fix, F1 phase-2
  pack pin) + Makefile port remain OURS (deferred); their worktree l2/
  Makefile edits theirs — merge later, flag don't silently resolve.
  Their worktree PROGRESS.md is their session ledger; ours supersedes for
  the shared mission (they agreed). They cross-verify ARMAGEDDON claims
  independently (campaign-J spirit); branch kept reviewable for user.
- LANE DISPUTE (22:15, RESOLVED 22:50 — L2-96 RETRACTED in ledger; recorded
  split STANDS: mission 05 = L2-72 lane; L2-96 confirmed DON'Ts: no
  fragment/patch edits, no main-tree kernel builds, no commits of our WIP;
  their kernel queue → RECOMMENDATIONS to our lane; l2-lowram folding into
  l2 = proposal only; user decision no longer required unless reassignment
  desired): L2-96 message CLAIMS
  the kernel implementation lane (kernel.fragment + patch-kernel-ram.py +
  main-tree kernel builds + missions 05/10) as THEIRS — contradicting
  their own prior reservation (~20:10 ledger: variant "does NOT touch
  kernel.fragment/patch-kernel-ram.py (mission 05 lane)"), L2-ea's 21:05
  confirmation (our lane), and maiake-f6's 21:55 ack. FACTS: our
  heavy-coder RUNNING (roster 22:10); fragment +32 lines mtime 20:45 =
  its LIVE batch-1 work (Kconfig-verified cpufreq/pstate/thermal/ACPI/
  vsyscall trims, well-commented); evidence 05-* (attribution tsv, batch1
  build EXIT=0 / forensics / rootfs / smoke32 logs, 05-kernel-size.md).
  NOT an abandoned handoff. L2-96 intends "verify then commit with
  attribution" — REJECTED (breaks mid-iteration verification; violates
  L2-ea commit ownership on campaigns/armageddon-ii; two concurrent
  ./l2 build kernel runs rm -rf the same source tree). Responses sent:
  L2-96 asked to honor recorded split or escalate to USER (only user
  reassigns lanes); no fragment edits / main-tree kernel builds / commits
  of our WIP from their side meanwhile. L2-ea + maiake-f6 flagged (don't
  accept L2-96 fragment commits; mission-05 results still from us).
  Protective: heavy-coder snapshots diff → 05-fragment-wip.patch per
  batch, ownership header in 05-kernel-size.md, foreign-edit detection →
  stop+report. USER NOTIFIED. L2-96 technical corrections relayed to our
  agent (PERF_EVENTS pinned by X86 select; PINCTRL_AMD bool; COMMON_CLK
  pinned via I2C_DESIGNWARE_PLATFORM; VIRTIO_NET stays y). Their additive
  build-lowram/ WP2 progress acknowledged (vmlinuz-l2-lowram +
  l2-lowram.iso produced, boot-verifying) — that lane respected. ISO
  delta resolution matches ours ✔. rcl2/l2pm two-claimant concern stale
  (we ceded to L2-ea 20:40).

## Log
- 2026-11-20 (session 2): ARMAGEDDON ledger created; heavy-coder definition
  restored; baseline ISO copied for sweep isolation; fresh 10M–1G sweep
  launched; mission 01 analysis started.
- 2026-11-20: sweep COMPLETE (16M–1G PASS, idle ~5.5 MiB flat; 10/12M FAIL).
  Missions 01–04 reports written. Root cause of low-RAM failure: ~7.3 MiB
  decompressed kernel image ⇒ ~8 MiB reservation floor. Config audit found
  unrequested built-ins (PERF_EVENTS, cpufreq stack via ACPI_PROCESSOR select,
  THERMAL, IOSF_MBI, ACPI extras, PINCTRL_AMD=y). Security posture noted for
  mission 61 (CPU_MITIGATIONS/SECCOMP/STACKPROTECTOR off). CGROUPS off ⇒
  mission 28 must use RLIMITs. heavy-coder launched for mission 05.
- 2026-11-20 (L2-96): full core rebuild exit 0 — default vmlinuz BYTE-IDENTICAL
  to baseline (sha256 0fa6cfc8…); new initramfs 421,275 B carries committed WIP
  rcl2 (17,184 B, selftest 48 KiB); new ISO 8.3 MiB boots at 16M. Failure-side
  forensics: 10M BIOS = silent triple fault (decompressor/initrd collision,
  init_size 0x7e4000 + PHYSICAL_START 0x200000 > usable top 0x9e0000; TCG
  trace #GP IP=0x9a1c90); RANDOMIZE_BASE=y confirmed ON; 10M direct = kthreadd
  OOM (managed 1152K); 12M = unpack OOM (managed 3212K); 16M used 5544K with
  Slab 3376K dominant. Report: build/campaigns/lowram-forensics-report.md
  (incl. adversarial flags on 8K stacks + skipped low-1M reservation for
  missions 91/93). Harness verified (36 tests) + committed by lead as 1888794;
  BIOS+UEFI matrix 10–256M running on ISO snapshot (lowram-default-matrix/).
  heavy-coder #2 launched for additive lowram kernel variant targeting 10M
  boot — lane: NEW files config/kernel.lowram + tools/l2-lowram + build-lowram/
  only; does NOT touch kernel.fragment/patch-kernel-ram.py/l2/build/ (mission
  05 lane). Worktree isolation was rejected (dirty tree), hence lane split.
- 2026-11-20 (session 2, 20:30): peer coordination resolved via shared ledger
  + cross-session messages. heavy-coder RESUMED on mission 05 (reserved
  lane) with peer's 10M triple-fault evidence (init_size collision) and
  contention-safe builds. rcl2 WIP measured: 17,184 B binary, 48 KiB selftest
  resident (mission 22 addendum). Kernel reproducibility verified
  byte-identical across peer rebuild; ISO delta = rcl2 content change.
  Explore agent mapping l2pm missions 41–50 still running. Next: integrate
  heavy-coder attribution/trim results + Explore gap map; advance campaigns
  C/E/F implementation in our lane; missions 91/93 to review peer's
  adversarial flags (8K stacks, skipped low-1M reservation).
- 2026-11-20 (session 2, 20:40): L2-ea (third session) announced rcl2/l2pm
  write scope; lanes accepted + recorded (see Concurrency). Our next
  implementation focus shifts to UNCLAIMED lanes: kernel 06–09 (patch-script
  analysis), boot-infra 14–19, userspace 31–40 audits, build-infra 51–60
  (careful: l2 script edits only when no peer ./l2 build is running),
  security 61–70 audits/fuzz tooling, perf 71–80 measurements, compat
  82/86–90 tests, review 91–100. Mission 71 partial baseline: kernel
  0→Memory: 1.93 s (32M direct boot, earlycon log); full cold-boot latency
  needs timed ISO runs (limine timeout 3 s + firmware included).
- 2026-11-20 (session 2, 20:50): Explore l2pm map COMPLETE; lead VERIFIED the
  three load-bearing claims against current l2pm.c (gc deletes inactive gens
  C:2586–2588; gc liveness DB-only ⇒ corrupt DB deletes live store; verify
  missing-manifest legacy pass C:2258; zero fsync/sig code). Gap map written +
  handed to L2-ea: build/campaigns/armageddon-l2pm-gapmap.md. Campaign E
  statuses updated (41/42/49 implemented-audited; 43/44/45/48/50 partial;
  46/47 missing — impl L2-ea). README ~67 KiB l2pm claim stale vs 88,184 B
  actual (doc defect → mission 98). L2-ea active: a2-* baseline logs +
  rcl2/verification/test-truncation.c (untracked, their lane). heavy-coder
  mission 05 still running.
- 2026-11-20 (session 2, 20:55): missions 31–33 audited →
  docs/armageddon/31-33-userspace-shell-libc.md. dash source URL is plain
  HTTP (sha256-pinned, TOFU first pin) — flagged for 53/69. Static-musl
  decision documented for mission 91. Next unclaimed work while heavy-coder
  runs: 51 build-script audit, 61/62 security posture reports, 14/15 repro
  initramfs + compression negative test, 71 timed ISO cold-boot runs.
- 2026-11-20 (L2-96, ~21:05): default-ISO characterization COMPLETE.
  Matrix 20/20 runs (10 PASS / 10 FAIL): BIOS floor 14M (13M OOM), UEFI
  floor ∈ (64M,128M] (64M = Limine high-memory-allocator PANIC in
  init_memmap, ANSI-decoded; 10–32M silent OVMF-stage). 14M pressure:
  sequential OK, parallel burst OOM-kills (PID1/shell survive); 16M same
  workload clean. S3 suspend/resume + poweroff + reboot verified clean at
  256M (low-1M-reservation flag downgraded to real-HW residual risk).
  Harness improved: ANSI stripping + limine-panic signature (first in
  order — its text contains "Out of memory"), 39 unit tests OK; changes
  uncommitted in L2-96 lane (tools/lowram_matrix.py, test_lowram_matrix.py).
  Full evidence: build/campaigns/lowram-forensics-report.md +
  lowram-default-matrix/ + build/lowram-forensics/. Remaining critical
  path: heavy-coder lowram variant (10M BIOS goal) still running; on land:
  verify claims, fold tools/l2-lowram into l2 (additive targets, coordinate
  lane), re-run matrix + test-stability.py (--settle 40) on l2-lowram.iso.
- 2026-11-20 (L2-96, ~21:15): CORRECTION to earlier L2-96 log entry — the
  harness was never in 1888794 (that commit = patch-kernel-ram.py +
  l2gpt/l2install/l2pm/l2wifi tests + armageddon-ii ledger). Harness + 39
  tests now committed by L2-96 as e24248c (only tools/lowram_matrix.py +
  test_lowram_matrix.py staged; peer WIP in config/kernel.fragment, l2pm,
  rcl2 left untouched). WP2 deliverables appearing in tree:
  config/kernel.lowram, tools/l2-lowram, build-lowram/ (untracked, agent
  still running — not committed yet).
- 2026-11-20 (session 2, 21:10): security posture audit (61/62/63/66/69
  partial) → docs/armageddon/61-69-security-posture.md. F1 VERIFIED (l2net
  find_pack no L2-LIVE requirement vs l2install:124,131; sr0-before-sd*
  ordering; rescue/direct-boot exposure; installed systems safe) → fix
  L2-ea W6, regression test mission 70. dash http:// at l2:293/327/683;
  l2 update empty-sha pin over HTTP (MITM-pin risk) → our build-infra fix,
  deferred while builds run. busybox login/passwd/su/getty present, boot
  bypasses auth (root console by design), no shadow → login/su untested
  (67). L2-ea lane confirmed + recorded; cd0f88a attribution corrected.
  L2-96 matrix COMPLETE noted (BIOS floor 14M; UEFI floor >64M — Limine
  init_memmap panic at 64M; S3/poweroff/reboot clean 256M) — UEFI low-RAM
  limitation feeds missions 84/90; our mission 05 trim also helps UEFI
  floor (smaller image → smaller Limine allocation pressure). heavy-coder
  mission 05 still running.
- 2026-11-20 (session 2, 21:25): L2-ea ACK — gapmap independently
  re-verified by them; torn rollback DB re-sync (C:2541–2545) confirmed as
  extra angle into the same live-store-deletion chain; relayed to their W2
  as P1–P3 with fail-before/pass-after regressions (P4/P5 deferrable,
  ed25519 design-only). W1/W2 commit hashes will be shared for our
  mission-92 re-stamp; README stale-size fix queued to their W5 (do not
  duplicate). Mission 51 PARTIAL DONE → docs/armageddon/51-build-scripts-audit.md:
  l2 orchestrator + Makefiles audited (repro hygiene good; 7 findings incl.
  dash HTTP resolver/empty-sha update pin, empty-pin TOFU cached-file
  acceptance, curl -L redirect downgrade, no download max-time, non-atomic
  multi-key lock update, limine GitHub-resolver/Codeberg-source mismatch).
  Fixes deferred until no peer ./l2 build runs (bash incremental-read
  hazard). l2install destructive-path/overlay/mkimage.sh review pending.
  heavy-coder mission 05 still running.
- 2026-11-20 (session 2, 21:30): L2-ea closures — F1 fix PHASING agreed:
  P1 their lane (l2net L2-LIVE marker + fail-closed search + l2install
  F3/F4/F5/F7/F14 + N3 set -f); P2 pack integrity manifest with hash
  PINNED IN INITRAMFS (boot-chain anchor; marker alone forgeable) — needs
  OUR l2 edits post-05, coordinate timing; installed-boot caveat recorded
  (no initramfs → pin on ext4 root via l2install). MODULE_SIG = kernel
  lane + user decision. Mission 70 regression ours after W6 hashes.
  udhcpc.script §66 item CLOSED: their W6 audit CLEAN + lead spot-check
  VERIFIED (charset validation, atomic resolv.conf, binary-derived quads;
  two moot micro-notes). Mission 66 → [x]. Mission 67 scope split: W1
  rcl2 parser fuzz/truncation (corpus share pending); ours login/su
  missing-shadow + non-rcl2 inputs. dash http:// cross-lane note acked
  (our fix, deferred). W6 finding IDs recorded for mission-92 re-stamp.
  heavy-coder mission 05 still running.
- 2026-11-20 (session 2, 21:40): FOURTH session maiake-f6 recorded (worktree
  campaigns/engineering only; its implementation agents redirected to
  verification/analysis). Lanes accepted: 06–09 analysis, 14–15, 51–53
  (worktree), l2wifi fuzz, release-matrix harness (60/97/100), guest-compat
  battery (82/86–90). Verified findings cited into missions 52/84 +
  Concurrency (a–g): ISO assembly byte-repro, l2pm cross-path repro
  (efc32819), UEFI floor ∈ (96M,128M] (sub-10M BIOS-only), NEW rcl2 /tmp
  uncapped-ramfs OOM/respawn defect (→ L2-ea; missions 19/29/93), l2net
  status exit-code fix (worktree; merge-coord with W6), l2install
  image-mode host-only guard recommendation (→ W6), NixOS make check
  musl-prerequisite fix (worktree; main-tree port queued OURS). Overlap
  flags recorded (l2net two-tree, 51 dual audit, main-tree l2/Makefile
  edits ours deferred). Roster check: heavy-coder RUNNING (mission 05);
  Explore completed. Four tui sessions active (L2-72=us, L2-96, L2-ea,
  maiake-f6) — user driving all four.
- 2026-11-20 (session 2, 21:50): F1 P2 design REFINED with L2-ea — caveat
  accepted; pin in TWO locations (initramfs /etc/l2/pack-hash at ISO build
  = our lane post-05; ext4 root pin via l2install = their lane). New
  motivation recorded: installed /lib/modules short-circuit is the only
  current guard — missing dir (kernel upgrade w/o modules/corruption)
  resurrects F1 on installed boots; root pin closes fall-through. Contract
  drafted for co-sign: docs/armageddon/p2-pack-pin-contract.md (per-pack
  manifests AGREED; host-install pin from build staging AGREED; lead
  counter-proposal: verify all module/bin files at activation + firmware
  at wifi-start instead of "files actually used"; initramfs rebuilt inside
  ISO step after pin injection; fail-closed chain; mission-70 regression
  requirements listed). Sequencing: W6-P1 immediate (theirs) → co-sign →
  P2 split (ours build-side post-05, theirs shell-side) → mission 70.
  heavy-coder mission 05 still running.
- 2026-11-20 (L2-96, ~21:55): L2-ea's six patch-review angles ALL dispositioned
  (tests + code reading; no patch edits — mission-05 lane). Results sent to
  L2-ea + build/campaigns/lowram-forensics-report.md §Adversarial review:
  (1) compile-time BASE_SMALL gating confirmed at 4G (pcp high=0 batch=7,
  min_free=128K; l2net fraction write ineffective) but 800-fork burst 0.8s
  zero failures → recommend runtime gating on totalram_pages (→05/80);
  (2) S3/SMP2/poweroff/reboot clean at 256M, real-FW residual; (3) vmemmap
  zero-tail invariant confirmed (<128M only) → recommend RO tail mapping
  (→05); (4) 8K stacks: stability 16M/32M PASS (VMAP_STACK guards; churn/
  fanout/ext4-writeback/recovery) → mission 89 partial evidence;
  (5) streaming initrd free SOUND (initrd_start advances; no double-free;
  412K = PAGE_ALIGN(421275) exact) — CORRECTION: retain_initrd=1 is a silent
  no-op by kernel design (bare flag only); bare-flag A/B verified (MemTotal
  −416K, /sys/firmware/initrd 421275 B root-only); (6) alloc_low_pages
  warning = benign memory_map_top_down fallback, no managed-page loss.
  Convergence: maiake-f6 UEFI floor (96M,128M] refines L2-96's (64M,128M].
  WP2 lowram variant still RUNNING (build-lowram/ lane).
- 2026-11-20 (session 2, 21:58): maiake-f6 ACK — 3 overlap flags resolved
  (l2net fix worktree-only cfad4c8, merge-coord W6; 51 dual audit
  cross-referenced with independent confirmations #1–#4 + nuance shipped
  dash_sha256 NON-EMPTY ⇒ HTTP exposure = update/TOFU pin path, added to
  our 51 report; kernel compile determinism between sessions 0fa6cfc8).
  Makefile musl-prereq port cleared to cite cfad4c8 (queued, deferred).
  BUDGET CORRECTION sent: release gate idle ≤15360 KiB (user band
  2026-11-13), 10240 = old default/stretch; ISO <10 MiB correct;
  mission-100 gate battery recorded. L2-96 adversarial review (6 angles,
  tests+code, no patch edits) noted as INPUTS to our lanes: runtime
  BASE_SMALL gating on totalram_pages recommended (→05/80, patch-script
  lane); RO vmemmap tail mapping recommended (→05/65); 8K stacks stable
  16M/32M under churn/fanout/ext4-writeback/recovery (→89 partial,
  91/93 flag downgraded); streaming initrd free SOUND + retain_initrd=1
  silent-no-op correction (bare flag design); alloc_low_pages warning
  benign. Full evidence: their lowram-forensics-report.md §Adversarial
  review. heavy-coder mission 05 still running; results to maiake-f6
  agent-D release matrix on landing.
- 2026-11-20 (L2-96, ~22:10): LANE AGREEMENT with L2-72 (lead): L2-96 now
  OWNS kernel implementation — config/kernel.fragment, tools/patch-kernel-ram.py,
  main-tree kernel/rootfs/initramfs/iso builds, <10 MiB boot goal (missions
  05/10 transferred; lead's heavy-coder stood down to read-only attribution,
  delivered build/campaigns/05-kernel-size.md — received, corrections noted:
  PERF_EVENTS pinned by X86 select, CPU_MITIGATIONS already n, PINCTRL_AMD
  bool, COMMON_CLK pinned via I2C_DESIGNWARE_PLATFORM, VIRTIO_NET must stay
  =y). Ping protocol: L2-96 pings lead before main-tree full rebuild or any
  rcl2/l2pm edit (L2-ea owns those; lead↔L2-ea overlap on rcl2/l2pm flagged
  to lead for sync). FLAG: config/kernel.fragment carries UNCOMMITTED
  mission-05 edits (mtime 20:45: SCHED_MC_PRIO=n cpufreq-stack drop,
  X86_PKG_TEMP_THERMAL=n, THERMAL_NETLINK=n, ACPI_SPCR_TABLE=n,
  ACPI_REV_OVERRIDE_POSSIBLE=n, X86_VSYSCALL_EMULATION=n) — lead's
  "untouched" claim was stale; L2-96 will PRESERVE + verify (measured
  build+boot) + commit with attribution after WP2 lands, unless lead
  objects. ISO delta for mission 52 RESOLVED: build/out/l2.iso ==
  lowram-default-snapshot d850fb81; vs baseline 06a4fd1e the only change is
  initramfs 426,833→421,275 B = committed rcl2 WIP cd0f88a (26,904→17,184 B);
  kernel byte-identical 0fa6cfc8. Rebuild confirmed complete 20:06:53 exit 0;
  only live kernel build is maiake-f6 worktree build-slubdebug (not main
  tree). WP2 (build-lowram/) has produced vmlinuz-l2-lowram + l2-lowram.iso,
  boot verification in flight. L2-96 kernel-lane queue post-WP2: verify
  fragment edits → FW_LOADER_COMPRESS_ZSTD=n (~83K) + TRIM_UNUSED_KSYMS=y
  (≤244K exports) measured batches → evaluate LTO_CLANG (LLVM=1) → matrix +
  stability re-run.
- 2026-11-20 (L2-96, ~22:50): CORRECTION — LANE TRANSFER CLAIM RETRACTED.
  L2-72's ~22:10 message proposed transferring kernel implementation
  (missions 05/10) to L2-96 and claimed their heavy-coder was stood down
  ("fragment untouched mtime 13:21"); L2-96 accepted in good faith (entry
  above). L2-72 has now RETRACTED: their heavy-coder is LIVE mid-batch
  (roster-verified 22:10; fragment +32 lines mtime 20:45 = batch-1 work,
  build EXIT=0, forensics+smoke32 done; the mtime-13:21 check was 20:20,
  pre-edit). Governing principle (agreed by all): ONLY THE USER can
  reassign lanes; peer proposals cannot. The user gave L2-96 the low-RAM
  boot mission, not the default-kernel trim lane. RECORDED SPLIT STANDS:
  mission 05 (kernel.fragment, patch-kernel-ram.py, main-tree kernel/
  rootfs/initramfs/iso builds) = L2-72 lane; L2-96 lane = additive lowram
  variant (config/kernel.lowram, tools/l2-lowram, build-lowram/ only) +
  lowram_matrix harness + forensics/verification. L2-96 confirmed DON'Ts:
  no fragment/patch edits, no main-tree kernel/rootfs/initramfs/iso builds,
  no commits of L2-72 agent WIP. The ~22:10 "kernel-lane queue" is hereby
  RECOMMENDATIONS to L2-72's lane (zstd/ksymtrim already passed by them;
  runtime-gating design + RO-tail in lowram-forensics-report.md §Adversarial
  review, L2-ea-endorsed; LTO feasibility: clang/lld 21.1.8, LLVM=1 THIN).
  Folding tools/l2-lowram into l2 = main-tree change → PROPOSAL only, not
  unilateral; variant stays standalone unless L2-72/user request integration.
  L2-96's ~22:05 "only live build is maiake-f6 worktree" was a point-in-time
  observation between L2-72 batches, not a stand-down claim. rcl2/l2pm
  two-claimant concern WITHDRAWN (L2-72 ceded to L2-ea 20:40). WP2 continues
  build-lowram/-only (separate L2_BUILD_DIR; cannot rm -rf build/src).
- 2026-11-20 (session 2, 22:55): DISPUTE RESOLVED — L2-96 retraction
  (~22:50) accepted; split stands (05 ours; their additive lowram lane +
  harness/forensics). Kernel queue received as RECOMMENDATIONS:
  zstd/ksymtrim already relayed (with module-export caveat); runtime
  BASE_SMALL gating + RO vmemmap tail (§Adversarial review, L2-ea-endorsed)
  → evaluate under 05/80/65; LTO_CLANG feasibility (clang/lld 21.1.8,
  LLVM=1 THIN) → mission 80 evaluation, not a 05 requirement. P2 CONTRACT
  CO-SIGNED (L2-ea A1–A3 + lead A2 extension, 22:25): uniform full-pack
  verification at FIRST activation (A1 closes the boot-probe firmware
  gap — wifi/NIC drivers request firmware during boot load_modules,
  before wifi-start); verifier-side manifest parse hardening + symlink/
  non-regular-file rejection (A2+ext); upgrade pin-refresh invariant
  generalized to l2net OR pack-content changes (A3); mission-70 cases
  added (wifi-HW boot cost, malformed-manifest, upgrade-without-refresh).
  Contract: docs/armageddon/p2-pack-pin-contract.md. Implementation
  queued: build-side ours post-05, shell-side L2-ea W6-P2 after ping;
  W6-P1 immediate theirs. heavy-coder notified (continue, protective
  steps stay); L2-96 acked. Mission 05 in progress (batch1 EXIT=0).
- 2026-11-20 (session 2, 23:00): maiake-f6 budget handling ACK —
  principled and correct: explicit --budget passthrough added to
  test-release-matrix.py (verified absent-by-default, 15360 reaches both
  sweeps; report header prints configured budget; L2_RAM_BUDGET_KIB works
  via ramtest). They KEEP strict 10240 default per THEIR user's brief +
  repo default — a cross-session message cannot change session
  requirements (same principle as the lane dispute); 15360 band flagged
  to THEIR user for explicit decision. Both thresholds pass today (idle
  ~5.5 MiB). Mission-100 gate semantics: cite worktree
  build/campaigns/11-regression.md 'Idle-RAM budget policy'; final idle
  threshold pending user harmonization of the two briefs (ours: 15360
  band per user 2026-11-13; theirs: 10240 strict). ISO <10 MiB hard gate
  agreed unchanged. Agent-D matrix gates: BIOS floor per our 05 trim,
  UEFI 128M+ regression, idle per confirmed budget, make check/doctor,
  artifact hashes. Mission-05 before/after package to them on landing;
  their double-build proof hashes coming after kernel-analysis builds.
  heavy-coder mission 05 still running.
- 2026-11-20 (session 2, 23:05): L2-96 formal STAND-DOWN ACCEPTANCE —
  dispute fully closed; recorded split stands (05 ours; their additive
  build-lowram/ lane + harness/forensics). DON'Ts confirmed; e24248c
  clarified (their own 2 files, L2-ea accepted). Recommendations logged:
  zstd/ksymtrim (relayed); runtime size-gating sketch + RO vmemmap tail
  (§Adversarial review, L2-ea W4-endorsed; PAIRING NOTE: runtime gating
  interacts with l2net VM-policy writes — coordinate with L2-ea W6
  whatever we choose); LTO_CLANG THIN feasibility (clang/lld 21.1.8,
  LLVM=1) → mission 80. Integration protocol: folding l2-lowram into l2
  = OUR lane; they won't act unilaterally; if we/user want integration,
  propose to them → they hand over delta; variant standalone meanwhile.
  WP2 build-lowram/-only (separate L2_BUILD_DIR, cannot rm -rf build/src).
  maiake-f6 second-matrix offer on variant ISO stands (read-only).
  heavy-coder roster-verified RUNNING 23:05; 05 results shared on landing.
- 2026-11-20 (session 2, 23:10): maiake-f6 NEUTRAL posture received —
  premised on STALE ledger read (dispute OPEN); corrected: pointed to
  RESOLVED 22:50 bullet + 22:55/23:05 entries + L2-96 formal stand-down
  acceptance; ownership resolved per recorded split, no user decision
  pending. Handling affirmed: gate ARTIFACTS not ownership, ISO sha256
  pinned in every matrix report (churn visible), no main-tree kernel
  writes, budget passthrough b5617f7 (strict 10240 default per their
  brief, 15360 flagged to their user), no cross-session relaying.
  Agent A 06-09/14-15 worktree analysis continues (their lane); reports
  to us for integration. heavy-coder running; 05 package pending.
- 2026-11-20 (session 2, 23:15): mission-05 progress check (lead).
  batch1 EXIT=0 (21:02, vmlinuz 21:02); batch2 attempt 21:07 logged
  configure+build-start but NO BUILD_EXIT (likely shell timeout); agent
  quiet 21:07–23:05 (queued-message processing/diagnosis). FRESH main-tree
  kernel build RUNNING since ~23:05 (-j6 -l5, drivers/net/wireless stage;
  source re-extracted, vmlinux not yet linked) — agent ALIVE, not stalled
  (roster running; initial stall assessment corrected). Lead created
  protective snapshot build/campaigns/05-fragment-wip.patch (73 lines;
  fragment +43/−1, patch-script unchanged) since agent hadn't yet.
  config/kernel.local verified BENIGN: user-owned tracked placeholder
  (Nov 12, 123 B, header comment only, no overrides) — applied no-op in
  both batches. Status query sent to agent (phase/deltas/remaining/
  blockers; don't interrupt builds). No intervention; monitoring. maiake-f6
  loop-closure acked (routing restored, agent-A data → our 05/80 + 06-09
  integration; L2-96 consumes same for variant).
- 2026-11-20 (session 2, 23:25): MISSION-92 RE-STAMP of L2-ea W1+W2
  (independent lead verification; build/campaigns/92-restamp-make-check.log).
  COMMITS VERIFIED on campaigns/armageddon-ii: 1984055 (rcl2 8 files
  +1350/−48: EFBIG truncation-safe reads, parser NUL/arg-limit rejection,
  rescue-newline fix — is_ws lacked '\n' so trailing l2.rescue=1 never
  matched; CONFIRMED in diff), 4acfd36 (l2pm +938: fail-safe gc closes
  live-store-deletion chain, 20 fsync additions, rollback intent marker,
  verify --repair reconciles torn DB/profile, missing-manifest "never a
  silent pass", transactions, set-aware remove), 1ec7140 (Makefile wiring).
  kernel.fragment/patch-kernel-ram: 0 TOUCHES ✔ (our WIP safe). MAKE CHECK
  re-run EXIT=0, 166 unit tests (17+14+45+19+3+4+33+17+10+4) + rcl2 C
  suites (supervision/truncation/lifecycle) green; new harnesses wired
  (test_rcl2_lifecycle.py 14 methods incl 37 fuzz + 6 PID1-namespace
  lifecycle; test_l2pm_transactions.py 33, was 25/33 fail-before). Sizes
  MATCH: rcl2 18,656 B, l2pm 100,472 B. NUANCES (honest): (1) rcl2 SELFTEST
  resident 48 KiB verified (unchanged); L2-ea "boot-resident 40 KiB" is a
  different path (pid1_boot vs selftest) — NOT independently re-measured;
  citation requested; mission 22 binary✔ selftest48✔ boot40 pending.
  (2) l2pm selftest RSS 168 KiB (budget 10240) — README ~130 STALE; W5/98
  use 168 KiB + 100,472 B (~98 KiB, not ~67). Statuses: 43/44/45/48/50
  IMPLEMENTED by 4acfd36 (gapmap P1–P3 closed, re-stamped); 25/29/30 rcl2
  hardening + suites (1984055); 67 W1 corpus received (ours keeps login/su
  + non-rcl2). INTEGRATION NOTE: build/out/rcl2 still OLD 17,184 B (peer
  20:06 rebuild); after 05 lands refresh build/out (./l2 build rcl2 l2pm +
  rootfs/initramfs/iso) so final ISO carries W1/W2; coordinate with W6-P1.
  W6-P1 launching (theirs); P2 waits our 05 ping. heavy-coder build running.
- 2026-11-20 (session 2, 23:30): L2-96 WP2 progress — lowram variant v1
  moved BIOS floor 14M→12M (serial-evidence re-verified); 10M gap
  quantified ~550–600K managed headroom; iteration in flight (their
  build-lowram/ lane). Integration protocol confirmed (post-05 proposal
  to user; standalone until then). Mission-70/93 regression additions
  (L2-96 suggestion, ACCEPTED): retain_initrd bare-flag semantics (=1
  silent no-op vs bare flag retains initrd: MemTotal −416K,
  /sys/firmware/initrd 421275 B root-only) + streaming-free initrd_start
  invariant (advances during unpack, no double-free, 412K =
  PAGE_ALIGN(421275)) — both patch-kernel-ram.py behaviors, guard in
  regression set. heavy-coder batch2 build running.
- 2026-11-20 (session 2, 23:35): L2-ea closure confirmation — independently
  corroborated dispute flag (HEAD 02cd24b, no fragment/patch commits from
  anyone, WIP live +43/−1, snapshot present). COMMIT-OWNERSHIP PROTOCOL
  agreed: our kernel.fragment/patch-script WIP committed ONLY when we
  declare verified+ready, with attribution, after L2-ea review;
  mid-iteration commits refused. P2 FINAL ACK confirmed (incl A2
  extension) — nothing pending either side; W6-P1 running, P2 shell-side
  after 05 ping. Dispute already resolved (L2-96 retraction 22:50 +
  acceptance 23:05); L2-ea surfacing in user report as transparency.
  W1/W2 re-stamp done 23:25 (nuances sent: rcl2 selftest 48 vs boot 40
  citation requested; l2pm RSS 168 vs README ~130 stale). heavy-coder
  batch2 running.
- 2026-11-20 (session 2, 23:45): mission-05 VERIFIED progress (lead
  spot-check of agent report + artifacts). BATCH1 (report+forensics
  confirmed): bzImage 2,921,472 (−36 KiB), init_size 0x7c9000 (−108 KiB),
  persistent image −104 KiB, reserved 8940K (−120K), Memory:
  22372K/32248K (4824K code), **10M BIOS collision CLEARED** (0x9c9000 ≤
  0x9e0000, 92 KiB margin), 32M smoke PASS idle 5500K, build 18m22s exit 0,
  ISO 8,744,960 B (<gate). BATCH2 (artifact-verified; report table
  pending): bzImage 2,827,264 (−128 KiB cumulative), init_size 0x77d000
  (−412 KiB cumulative), 10M margin ~396 KiB; applied TRIM_UNUSED_KSYMS=y
  + FW_LOADER_COMPRESS_ZSTD=n. SAFETY GATE sent to agent: ksymtrim
  module-load MUST be empirically verified (e1000/wifi modalias, no
  Unknown-symbol; smoke32 insufficient) — revert ksymtrim if modules
  break (mission-93 gate). OPERATIONAL guidance sent: ALL long builds
  is_background:true (foreground 600s cap likely killed earlier attempts
  = root cause of quiet gaps). ≥500 KiB target not yet met (−104 KiB
  persistent at batch1; batch2 deltas pending) — 500 is target not gate;
  complete verification + floor analysis even if plateau. Agent running;
  report final-verification/floor sections empty; snapshot present.
- 2026-11-20 (session 2, 23:55): maiake-f6 agent A DONE — verified data
  received (worktree build/campaigns/01-kernel-config.md, 02-boot-memory.md,
  08-slab-data.txt, 07-pagetable-data.txt, 14-initramfs-repro.txt + logs).
  MISSION EVIDENCE CLOSED (cite A): 14 initramfs double-build identical;
  15 xz/zstd initramfs cleanly rejected ("decompressor failed", controlled
  VFS panic — gzip-only validated); 09 initmem 692K freed + streaming
  initrd free firing; 17 10M root cause independently proven (init_size
  0x7e4000 + PHYSICAL_START 0x200000 = 0x9e4000 overruns Limine top-of-RAM
  boot_params/initrd → #GP @0x9a1c90 → triple fault; KASLR EXONERATED);
  52 third independent compile-determinism confirmation (0fa6cfc8,
  2958336 B, 0 dropped). INPUTS relayed to heavy-coder: B2a zstd=n −88K
  safe (applied); B2b ksymtrim fraction-of-238.6K (gate pending); B3
  kernfs_node_cache ~1.3MiB/9780 nodes UNRECLAIMABLE device-count-driven
  (SLUB_DEBUG ranking not absolute under TINY) → highest leverage sub-10M,
  missions 08/80; safe trims ETHTOOL_NETLINK=n ~113K (confirmed =y),
  ACPI_AC/BATTERY→m ~20K; CAUTIONS: PERF_EVENTS PINNED (confirmed
  arch/x86/Kconfig:320 select — A's 178K trim estimate INVALID, correction
  sent to maiake-f6), HOTPLUG_CPU needed by SUSPEND, FONT_TER16x32/
  PCIEPORTBUS hi-dpi/real-HW trade-offs, MMC/I2C kept; PHYSICAL_START
  ≥0x200000 hard floor (Kconfig range + MIN_KERNEL_ALIGN BUILD_BUG_ON) —
  span only lever; capacity model managed≈RAM−1149K−span+init_freed,
  runtime ~3.9MiB, 10M needs span<5.9MiB (batch2 7.5MiB → ~1.6MiB more),
  8M span<3.6MiB below essential floor (not realistic); LTO_CLANG est
  5–15% (~250–730K) → mission 80 not 05. A confirmed shared files
  untouched; additive slubdebug committed on their branch. 06/07/08 data
  available for verification. heavy-coder finishing batch2 gate + verif.
- 2026-11-20 (session 3, 00:05): mission-05 FINAL SWEEP lead-VERIFIED
  (build/campaigns/05-final-sweep.log; trimmed build/out/l2.iso; BIOS
  -smp2 settle40 budget15360): 10M FAIL (OOM, collision cleared), 12M FAIL
  (OOM), 14M PASS 5516/6548, 16M PASS 5536/8596, 32M PASS 5580/24724,
  256M PASS 5524/250524, 1G PASS 5564/1024668. Idle FLAT ~5.5 MiB, all
  under budget. Trim gained ~+404 KiB managed at EVERY size (16M MemTotal
  8192→8596). Default BIOS floor STILL 14M (12M managed ~3616K < ~3.9 MiB
  runtime need); 10M now OOM-limited NOT triple-fault. batch2 build EXIT=0
  (bg shell bg_d6153336 confirmed). PENDING before ready/commit: ksymtrim
  module-load gate, UEFI 256M, earlycon forensics+init_size, double-ISO
  sha256, make check, doctor, report final sections (empty). Agent scratch
  rootfs build/campaigns/05-* (21:3x) present. note/qwen4.txt updated with
  sweep + closure addendum (user-requested handoff, 489 lines).
- 2026-11-20 (session 3, 00:10): maiake-f6 VERIFIED PERF_EVENTS correction
  in-tree (arch/x86/Kconfig:320 unconditional select; kernel/power/Kconfig:150
  HOTPLUG_CPU select under SUSPEND). A's 178K PERF_EVENTS trim STRUCK.
  Corrected safe-trim total ~417K (was 595K): cleaner candidates
  ETHTOOL_NETLINK=n ~113K + ACPI_AC/BATTERY→m ~20K + INTEL_IDLE (remainder
  of CPU_FREQ/THERMAL/INTEL_IDLE 103K; cpufreq/thermal done batch1);
  HOTPLUG_CPU/SMT ~30K CONDITIONAL on dropping SUSPEND (don't);
  FONT_TER16x32 16K + PCIEPORTBUS 35K documented hi-dpi/real-HW trade-offs;
  MMC/I2C kept. maiake-f6 revived agent A to re-verify EVERY candidate vs
  Kconfig selects → authoritative erratum table coming in worktree
  build/campaigns/01-kernel-config.md. Relayed to heavy-coder: WAIT for
  erratum before batch3; FINISH verification first (module gate/UEFI/
  forensics/double-ISO/make check/doctor/report); batch3 OPTIONAL,
  500 KiB target-not-gate. Dispute CLOSED confirmed by L2-ea (N5 names us
  kernel-side executor; W6-P1+W7 running; WP2 v1 variant floor 14M→12M
  bzImage −736K 10M managed 3200K). note/qwen4.txt addendum updated.
- 2026-11-21 (L2-54, resumed L2-96 ref 96af98, 12:35-12:45): ★ VARIANT v3
  BOOTED 10 MiB (WP2 evidence Nov 20 21:38, parent-verified on disk):
  build-lowram/campaigns/v3-10M/results.json PASS — MemTotal 5440K, used
  3836K, reserved 4800K, spawn_ok TRUE. bzImage 1,647,616 B (−44% vs
  default 2,958,336), ISO 5,961,728 B (5.69 MiB). Trend at 10M: managed
  1152→3200→4044→5440K; reserved 8568→6520→5676→4800K. v3 lever hint:
  VT=n + variant initramfs with tty1 service removed (initramfs-l2-lowram
  421,568 B, +293 B vs main) — full attribution in WP2 report pending.
  WP2 was PAUSED at session break; RESUMED with continuation: finish BIOS
  12/16/32/256 sweep + UEFI 128/256 + poweroff + idle-band + double-build
  sha256 + v2→v3 attribution + report; l2wifi trim optional polish (not
  needed for 10M). Mission 10 stays [~] until parent independent
  re-verification (39-test harness OK post-merge; 9M strictly-under probe
  to add). Overnight user git: a8a2849b5 bulk-commit (variant files +
  build-lowram tree now tracked), b1774e8c3/db137655d gitignore, 1910fe1e7
  merge main (main still cd0f88a) PUSHED origin/campaigns/armageddon-ii;
  backup/armageddon-ii-pre-merge exists. Observed 12:40: active peer
  (L2-aa?) restructuring main-tree l2 (+61/−25, 'build hardware' target
  referenced by tools/l2-lowram comment) + README/.gitignore — main-tree
  lane, not touched by me; config/kernel.lowram reverted to committed v3
  state (actor unattributed); tools/l2-lowram small diff (L2_MAIN_BUILD_DIR
  override + iso output-path arg). Full session record: note/qwen3.txt
  (§13 resumption). FINAL chain partners (L2-ea rcl2 refresh, maiake-f6
  second matrix) sessions not currently listed — re-coordinate when
  reached; agreements stand recorded.
- 2026-11-21 (L2-54, ~12:50): WP2 SCOPE VIOLATION DETECTED + CORRECTED.
  mtime attribution: l2 + tools/l2-lowram written 12:35:43 within 234µs
  (automated write at WP2 resume), README 12:35:09, .gitignore 12:40:37.
  l2 diff added lowram()/build_lowram() integration AND switched dashboard
  kernel/initramfs/iso rows to LOWRAM artifacts — product decision reserved
  to user/lead, outside WP2's variant-only scope and the recorded
  integration-as-proposal agreement. Correction issued: preserve as
  build-lowram/proposals/l2-lowram-integration.patch → git checkout -- l2
  README.md .gitignore (revert to 1910fe1e7) → verify clean; in-scope
  tools/l2-lowram diff kept; v3 sweep continues (v3-bios 12/16/32/256M
  running). WP2 to report violation+correction in final report Risks.
  L2-aa handshake (restored L2-ea ref eaaab2): lanes confirmed NO overlap;
  attribution correction sent (l2/README not my intended lane); agreements
  re-confirmed: FINAL sequencing (post-W7 rcl2 refresh after their
  test_mempressure --ram 16M gate; hash+size shared before my re-copy +
  selftest re-stamp + sha pin), N5 three-party threshold, P2 co-sign,
  mission-92 re-stamp. L2-aa status: W1/W2 rebased e34ab3d43/264d82b86,
  W6/W7 fixes committed by user d1959a232, their two heavy-coders resumed
  for verification gates, make check EXIT=0. Revert completion to be
  verified by parent at WP2 next boundary.
- 2026-11-21 (L2-54, ~13:00): CORRECTION to entry above — make check is
  EXIT=2 FAILED, NOT 0 (L2-aa misread wrapper exit; corrected in their
  commit 1f976e7d7 + their ledger): test_l2install 7 finding-tests red
  (f3_host/f3_live/f3_cpio/f4/f5/f7/f15) = W6 l2install fixes INCOMPLETE
  (TDD fail-before committed during interruption); rcl2(W7)+l2pm(W2)+
  l2gpt GREEN. W6 resumed to green; W7 mempressure gate pending. FINAL
  INTEGRATED ISO requires W6 green + W7 gate PASS. L2-aa orphaned-lane
  flag (their c56a828ae) ACKNOWLEDGED: with L2-72 gone, default-kernel
  mission-05 iteration + P2 build-side + N5 kernel-side finalization +
  FINAL integrated ./l2 build have NO active executor; surfaced to MY
  user in plain text for decision (assign L2-54 / assign L2-aa / wait
  L2-72 / leave paused). L2-54 remains VARIANT-ONLY per recorded split
  (only the user reassigns); variant FINAL proceeds independently;
  integrated ISO blocked pending executor. L2-aa P2 shell-side + N5
  l2net side paused against ownerless finalization ping — correct
  conservative posture. v3-bios SWEEP 4/4 PASS (WP2 evidence,
  build-lowram/campaigns/v3-bios/): 12M 7488/3812/4800, 16M
  11520/3812/4864, 32M 27648/3816/5120, 256M 253448/3832/8696 (K,
  total/used/reserved), all spawn ok; idle ~3.8 MiB flat (default 5.5M).
  v3-uefi sweep running. WP2 scope-revert (l2/README/.gitignore →
  proposals patch + checkout) still pending at next tool boundary;
  parent will verify. note/qwen3.txt §13 current.
- 2026-11-21 (L2-54, ~13:10): (a) SCOPE-REVERT VERIFIED COMPLETE —
  build-lowram/proposals/l2-lowram-integration.patch preserved;
  l2/README.md/.gitignore CLEAN at HEAD (git status confirmed). (b) W7
  COMPLETE (L2-aa, lead-verified+accepted) + FINAL INPUT VERIFIED by
  parent: post-W7 rcl2 = rcl2/build/rcl2 15,568 B sha256
  07fb2c69a9b179f274a5fff96a32a88e8b411e4fb910f65064cdd48192ee3a3b
  (matches L2-aa published prefix; build/out/rcl2 still pre-W7 17,184).
  Gate evidence: unedited test_mempressure 16M exit 0 (reclaim
  5608→5536, L2ALIVE, no OOM, PID1 VmRSS 32 kB, clean poweroff), ENOSPC
  at exactly 2048 KiB, live-install 256 RC=0, idle +20/+44 ≤64,
  boot-resident 36 KiB, caps /tmp 16384 /run 8192, make -C rcl2 test
  EXIT=0 (5 suites). WEDGED EXPECTATION RETIRED — variant verification
  adopts the unedited mempressure 3-part gate post-refresh. W6 still RED
  (make check EXIT=2; l2install/l2net M = their agent implementing).
  (c) v3-uefi COMPLETE: 16M FAIL-silent (firmware floor, expected),
  128M PASS 118500/3972/12572, 256M PASS 247524/3956/14620, spawn ok;
  poweroff-16M done; v3-direct 10M running; double-build + attribution +
  report remain. (d) FINAL RE-COPY QUEUED (after WP2 declares v3 final):
  post-W7 rcl2 → tools/l2-lowram initramfs+iso (L2-54 lane) → selftest
  re-stamp → re-verify 10M + subset + mempressure → FINAL sha pin →
  maiake-f6 ping (not in roster; via ledger/worktree). Margin: −1,616 B
  shmem + −12K boot anon = polish (v3 passes 10M with ~1.6M available).
  Orphaned-lane user decision still pending (default integrated ISO
  blocked; variant FINAL independent). note/qwen3.txt §13 current.
- 2026-11-21 (L2-54, ~13:25): v3 ATTRIBUTION CORRECTION (parent-verified,
  3 independent lines) + REPRO PASS. Actual v3 build config = EXT4_FS=n,
  INET=n, NETDEVICES=n, ETHERNET=n, VIRTIO_NET=n, SMP=n (NR_CPUS=1), VT=n.
  Committed HEAD config/kernel.lowram + build-lowram/v3/kernel.lowram.v3
  (EXT4=y/INET=y/VIRTIO_NET=y) = unbuilt FUNCTIONAL DRAFT captured by bulk
  commit a8a2849b5 — preservation mislabeled; WP2 instructed to save the
  actual build fragment as v3, relabel the draft, explain the divergence,
  and report trade-offs prominently; NO further config edits pending USER
  decision. Evidence: repro bzImage == v3 sha
  4d38a2a76248065af70d8f9bd73d5f65f624604a48c24eb0eaea291c20048530
  (1,647,616 B — double-build REPRODUCIBILITY PASS); repro .config from
  working fragment has EXT4/INET/SMP/VT =y count 0; v3 footprint
  (16M 11520/3812/4864, idle ~3.8M flat) consistent only with stripped
  config. TRADE-OFFS (brief KEEP-list deviation, USER DECISION PENDING):
  no ext4 → l2install disk impossible from variant; no INET/NIC → no
  TCP/IP/DHCP (lo/UNIX/PACKET remain); 1 CPU; serial-only. Preserved:
  shell/fork/exec/ISO9660/virtio-blk raw/ACPI poweroff. v2 (12M PASS)
  retained ext4+INET = functional option; EXT4-only re-add ≈ +1 MiB
  floor. v3 suite complete: BIOS 10/12/16/32/256 PASS; UEFI 16M
  FAIL-silent (firmware floor) / 128M+256M PASS; direct 10M PASS
  (5440/3828/4800); poweroff clean. l2net stat_refresh nit → L2-aa W6.
  Earlier L2-54 claims 'committed = v3 state' and 'initramfs unchanged'
  CORRECTED (report + note/qwen3.txt §13). Variant FINAL still proceeds:
  WP2 report → parent re-verification → post-W7 rcl2 re-copy → 9M probe →
  mempressure gate → sha pin.
- 2026-11-21 (L2-54, ~13:35): W6 DONE (L2-aa, lead-verified+accepted,
  commit ac62c0101) — ALL L2-aa workstreams W1/W2/W6/W7 complete.
  make check INDEPENDENTLY VERIFIED GREEN by L2-54 on settled tree:
  EXIT=0, 181 unit tests / 11 suites OK + rcl2 selftest ok (resident
  48 KiB ≤ 150 budget; L2-aa reported 44 — ±1 page measurement variance,
  same binary sha). Log: build/campaigns/lowram-make-check-verify.log.
  CORRECTIONS to earlier L2-54 entries: (a) d1959a232 contained W7 fixes
  + W6 TEST HARNESS only — W6 SCRIPT fixes were NOT in it (scripts were
  byte-identical to pre-fix HEAD, hence resume EXIT=2); W6 scripts landed
  post-resume in ac62c0101. (b) cpio audit semantics CORRECTED (L2-aa W6
  empirical): busybox cpio STRIPS leading '/' by default but REJECTS
  --no-absolute-filenames; GNU cpio does NOT strip by default but ACCEPTS
  the flag → F3 containment was a REAL defect on GNU hosts (absolute
  member escaped on HEAD); fixed via runtime support probe (GNU gets the
  flag; live busybox flow byte-identical). All 8 findings fixed
  (l2install F3/F4/F5/F7/F14/F15 + l2net N1/N3), 25/25 lane tests,
  fail-before/pass-after logged, test_l2net wired into check. N1 marker
  gate landed; P2 step-2 slot commented in find_pack (ready for pin
  verification when build-side executor assigned). INTEGRATED ISO now
  unblocked on L2-aa side; ONLY blocker = orphaned integrated-build
  executor (USER decision pending). Post-W7 rcl2 re-verified intact:
  rcl2/build/rcl2 15,568 B sha 07fb2c69…3a3b (make check did not rebuild
  it); committed rcl2/Makefile carries W7 recipe (-Oz, no-unwind-tables,
  force-unwind-tables=no) → re-stamp at FINAL will rebuild from committed
  source and compare sha. Variant FINAL awaits WP2 report only.
- 2026-11-21 (L2-54, ~14:50): PRESERVATION FIX VERIFIED — WP2 processed
  the correction (14:46): build-lowram/v3/kernel.lowram.v3 == working
  fragment (diff clean); draft relabeled functional-draft-NOT-BUILT.
  AUTHORITATIVE build-time .config (v3/kernel-lowram.config) confirms
  stripped symbols directly: SMP/INET/VT/EXT4_FS not set, VIRTIO_NET/
  NETDEVICES absent; EFI_STUB/ACPI/PACKET/UNIX/VIRTIO_BLK/
  SERIAL_8250_CONSOLE/ISO9660 kept. v3 attribution now proven from the
  build config, not inference. repack-initramfs.py located (L2-aa
  clarification): maiake-f6 worktree rcl2/verification/, W7 used unedited;
  main-tree depth-3 search correctly missed it. v3-direct 10M PASS
  (5440/3828/4800 spawn ok) shared. 9M probe pending FINAL suite.
  WP2 report expected via completion notification; then parent
  re-verification → post-W7 rcl2 re-copy → 9M + mempressure → sha pin.
- 2026-11-21 (L2-54, ~15:00): WP2 FINAL REPORT received + reconciled
  (report + note/qwen3.txt §13). Goal verified: 10M BIOS L2#+spawn;
  bzImage 1,647,616 B sha 4d38a2a7… (2-build repro PASS, .config too);
  init_size 4.75 MiB; vmlinux 4.29 MiB; idle 3.74 MiB flat 12-256M;
  poweroff clean; UEFI 128/256 PASS / 16M firmware-floor FAIL. Full v3
  levers: EXT4/INET/NIC/SMP/VT + BUG=n, BLK_DEV_SD=n, ACPI_BUTTON=n,
  INPUT=n, SWIOTLB=1M; MICROCODE/PERF_EVENTS FORCED. initramfs +293 B
  attributed (rcl2.conf −51 B tty1 line; newc identical 782,336 B; repack
  deflate artifact). Timeline: committed HEAD fragment = unbuilt v4
  draft (interrupted at extraction); working tree = true v3. SECURITY:
  VM-only throwaway (mitigations/KASLR/IBT/PTI/retpoline/BUG off) — NEVER
  bare metal/untrusted hosts. CORRECTION: functional 12M alternative =
  v1 (INET+EXT4+SMP+VT kept), NOT v2 (v2 dropped INET kept EXT4) —
  relayed to L2-aa for release-readiness.md. Refresh (post-W7 rcl2)
  disk-verified: initramfs 420,933 B, ISO rebuilt, kernel sha unchanged;
  WP2 sanity PASS 10M 5440/3816/4800 + 16M 11520/3824/4864 (used −20K
  post-W7 ✓). Parent independent FINAL suite RUNNING (14:58:25): 9M
  probe + 10/12/16/32/256 subset + unedited mempressure 16M + sha pin →
  build/campaigns/lowram-final-verify/. User decisions pending (posture
  + orphaned executor); WP2 config frozen pending posture decision.
- 2026-11-21 (L2-54, ~15:05): ★★★ VARIANT FINAL VERIFICATION COMPLETE —
  ALL GATES PASS (parent-independent, committed harness e24248c,
  refreshed post-W7 ISO) ★★★
  BIOS 6/6 PASS MATRIX_EXIT=0: **9M 4416/3812/4800 spawn ok (STRICTLY
  UNDER 10 MiB — GOAL EXCEEDED; capacity model predicted MemTotal 4416K
  EXACTLY)**, 10M 5440/3812/4800, 12M 7488/3808/4800, 16M
  11520/3812/4864, 32M 27648/3832/5120, 256M 253448/3820/8696; idle
  flat 3.72-3.74 MiB. MEMPRESSURE_EXIT=0 (unedited worktree gate, 16M):
  PID1 VmRSS 36 kB, NO OOM — dd hit ENOSPC at /tmp cap (W7 caps convert
  OOM-kill → clean capacity limit), reclaim verified (+40K), survived.
  8M PROBE: FAIL silent 4.1s → FLOOR BRACKETED 8M FAIL / 9M PASS
  (mechanism uninvestigated; optional TCG follow-up). SHA PIN VERIFIED
  == WP2 published: ISO
  01c784a1ce6fca03d36d4090b6f4b20098680972833350c7c70958f351fd0c3d
  (5,961,728 B), kernel
  4d38a2a76248065af70d8f9bd73d5f65f624604a48c24eb0eaea291c20048530
  (1,647,616 B), initramfs
  a6974f5aba2209eb3ac09ef0676ce7e76e7eff653398adcb44ae9680f28c91ce
  (420,933 B). Preservation COMMITTED 9da900ab2 (true-v3 fragment +
  L2_LOWRAM_RCL2 override; commit == built artifact). Evidence:
  build/campaigns/lowram-final-verify/{results.json,summary.md,SHA256SUMS,
  per-size logs} + lowram-final-verify-8m/ + lowram-forensics-report.md
  §PARENT INDEPENDENT FINAL VERIFICATION. WP2 suite (pre-refresh):
  UEFI 128/256 PASS, 16M firmware-floor FAIL; poweroff clean; 2-build
  repro PASS. MISSION EVIDENCE CHAIN COMPLETE.
  **PING maiake-f6 (second-matrix agreement)**: FINAL ISO is
  build-lowram/out/l2-lowram.iso sha 01c784a1… (above); run your
  8/10/12/14M bracket read-only per agreement (my 8M FAIL/9M PASS
  already measured — independent confirmation welcome); report to your
  worktree build/campaigns/, L2-54 will cite. Kernel
  build-lowram/out/vmlinuz-l2-lowram sha 4d38a2a7… for direct-mode
  comparison if desired.
  USER DECISIONS PENDING: (1) variant posture — extreme 9-10M shell
  (v3, VM-only security caveat) vs functional 12M (v1, INET+EXT4+SMP+VT)
  vs both; (2) orphaned-lane executor (default-kernel mission-05
  iteration + P2 build-side + N5 kernel-side + integrated ./l2 build).
  Variant config frozen pending (1). L2-aa release-readiness.md carries
  9M measured headline + trade-off/security caveats.