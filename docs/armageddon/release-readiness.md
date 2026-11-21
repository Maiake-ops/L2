# L2 ARMAGEDDON II — Release Readiness (W5, missions 281–300)

Status: **DRAFT** · Author: L2-ea / L2-aa (lead) · 2026-11-21
Scope: release-engineering summary of VERIFIED campaign state. Data is
labelled **MEASURED**, **PREDICTED**, or **PENDING**. Companion live ledger:
`docs/armageddon-ii-progress.md`; session record: `notes/qwen1.txt`.
This doc documents verified state; it does NOT authorize a release. The
integrated default ISO release is BLOCKED on an orphaned-builder decision.

---

## 1. Two-kernel comparison (headline)

L2 now ships two kernel configurations with distinct trade-offs:

| Property | DEFAULT (`config/kernel.fragment`) | VARIANT (`config/kernel.lowram`, `l2-lowram`) |
|---|---|---|
| Purpose | real-hardware breadth (NVMe/AHCI/USB/HID/PS2/EFI-fb/ACPI, modules) | ultra-low-RAM boot, minimal surface |
| Modules | `MODULES=y` (hw pack on medium, modalias load) | `MODULES=n` (no module loading; no hw pack) |
| Idle RAM (MEASURED) | ~5.5 MiB (16M used 5564K, 24M 5632K) | **~3.8 MiB FLAT** across 12–256M (used 3812–3836K) |
| BIOS boot floor (MEASURED) | **14M** (13M OOM-deadlock) | **10M** (v3 PASS; v1/v2 10M FAIL) |
| UEFI boot floor (MEASURED) | (96M,128M] — Limine `init_memmap` panic below | 128M PASS, 256M PASS; 16M FAIL-silent (firmware-bound) |
| Decompressed image | 7490 KiB (4891 code/351 rwdata/1392 rodata/692 init/164 bss) | reduced (bzImage −736K at v1; v3 lever report pending) |
| Module-injection surface | present → W6 marker gate + P2 pin (pending) | **removed** (MODULES=n, no pack) |
| Core ISO size (MEASURED baseline) | 8,787,968 B (8.38 MiB < 10 MiB gate) | variant ISO pending FINAL pin |

**Headline:** the variant boots at 10 MiB total guest RAM with ~3.8 MiB
idle — flat across guest sizes — meeting the campaign's primary low-RAM
boot target. The default kernel retains real-hardware breadth at a 14M
BIOS floor. UEFI low-RAM is bootloader-bound (Limine), not kernel-bound,
for both.

### <10M stretch — PREDICTED (not yet measured)
L2-54 capacity model + v2/v3 evidence: at **9M**, present ≈8700K, reserved
floor ~4800K → MemTotal ≈4416K, used ~3836K → available ~580K. This is
+372K over v2's fatal managed level (4044K at 10M), so unpack + PID1 should
complete → **9M predicted BOOT**. **8M predicted FAIL** (managed ≈3392K <
4044K). The stretch likely lands at 9M, not 8M. **The FINAL-suite 9M probe
will settle this; do NOT claim 9M boots until measured.**

---

## 2. PID 1 (rcl2) — MEASURED

| Metric | Value | Source |
|---|---|---|
| Binary size | **15,568 B** (sha256 `07fb2c69…3a3b`) | post-W7 build |
| Selftest RSS | 44 KiB (budget 150) | `make -C rcl2 test` |
| Boot-resident (VmRSS) | **36 KiB** | W7 boot proof / mempressure gate |
| Live scratch caps | /tmp 12.5% RAM clamp 2–16 MiB; /run 6.25% clamp 1–8 MiB | sysinfo(2); at 256M → /tmp 16384K, /run 8192K |
| Size carries | -Oz, force-unwind-tables=no, rss_report→g_iobuf, probe-byte refinement | all kept, none regressed |

Hardening landed (W1+W7): truncation-safe reads (EFBIG probe; config→
default shell, cmdline→complete-token trim, mounts→streaming scanner);
parser rejects NUL lines / >6 args / overlong args; rescue-newline fix
(`is_ws` lacked `\n` → last-token `l2.rescue=1` never matched); D5
`console_has_ttys` OOB read (CWE-125) bounded; live `/tmp`+`/run` capped
tmpfs (ENOSPC before global OOM — closes the 16M console wedge).

**W7 locked gate (MEASURED, PASS):** unedited `test_mempressure.py --ram
16M` → ENOSPC at exactly 2048 KiB, used 5608→5536 KiB reclaim, shell
survives, no OOM, PID1 VmRSS 32 kB, clean poweroff. `test-live-install
--ram 256` → RC=0 + installed disk boots to L2#. Idle delta +20/+44 KiB
(≤64). Evidence: `build/campaigns/a2-w7-*.log`.

---

## 3. Package manager (l2pm) — MEASURED

| Metric | Value |
|---|---|
| Binary size | 100,472 B (was 88,184; +12 KiB) |
| Selftest RSS | 168–172 KiB (budget 10240) |
| Tests | 104 (71 existing + 33 new); 25/33 fail-before |

Hardening landed (W2): **fail-safe gc** (refuses on unreadable DB/gen
metadata before any deletion; liveness = DB + kept generations; keep
active + newest 2/profile; `--prune-generations` legacy opt-in) — closes
the verified power-loss→corrupt-DB→gc-deletes-LIVE-store chain; fsync
file+dir at all 19 temp+rename sites; rollback intent marker + `verify
--repair` (DB-from-generation reconstruction, missing-manifest never
silent-pass, divergence rebuild; store never modified); transactions for
add/install/remove; set-aware multi-remove; dep-version + profile-conflict
warnings. Single-writer assumption documented (concurrency stress queued).

**PENDING:** `l2pm/Makefile` build-path alignment (`-ffunction-sections
-fdata-sections --gc-sections` to match `l2 build l2pm`) — orphaned
(maiake-f6 lane, session gone). Final binary size/RSS may change; README
size refresh held until this settles. Signatures (ed25519 sidecar) +
binary repo index = design proposals only (not implemented).

---

## 4. Installer / network safety (W6) — MEASURED

8 findings fixed in `l2install` + `overlay/bin/l2net` (commit `ac62c0101`),
25/25 lane tests green, fail-before/pass-after logged:
- **F3** `setup_live` dies unless `/proc/1/comm=rcl2` (no env bypass; host
  with build tree unchanged) — closes silent root execution of install
  tools from an inserted medium on hosts. cpio `--no-absolute-filenames`
  via runtime probe (GNU gets it; busybox rejects the flag but strips
  leading `/` by default → live flow byte-identical).
- **F4** refuse existing non-empty image target without `--yes`.
- **F5/F14** cleanup umounts only what we mounted (flags); live keeps medium.
- **F7** strip `\n\r|` from USB MODEL after `\x20` unescape (picker injection).
- **F15** image mode host-only guard (busybox lacks truncate/losetup).
- **N1** l2net requires `L2-LIVE` marker before accepting any pack (both
  pre-mounted and device-loop paths); installed `/lib/modules`
  short-circuit + early-exit trim preserved. P2 step-2 slot commented.
- **N3** `set -f` around modalias split.

**CPIO audit correction (empirical):** busybox strips `/` by default but
REJECTS `--no-absolute-filenames`; GNU does NOT strip by default but
ACCEPTS it. F3 containment was a REAL GNU-host defect (absolute member
escaped on HEAD), fixed via the probe.

**P2 pack-pin contract (co-signed, NOT implemented):** per-pack manifest +
dual-location `/etc/l2/pack-hash` pin (initramfs + ext4 root); A1 uniform
verify-all-at-first-activation (in `use_pack` before modprobe/exec/firmware);
A2 manifest parse hardening + regular-files-only; A3 atomic pin refresh on
any l2net/pack change. Build-side orphaned (l2 lane); shell-side ready,
pauses for the build-side executor. See `docs/armageddon/p2-pack-pin-contract.md`.

---

## 5. Security posture (honest, two kernels)

Kernel hardening trade-offs (default config; **user/campaign-J decision**
to change — each trades against the 10 MiB ISO / 15 MiB idle budgets):
- OFF: CPU_MITIGATIONS (Spectre V1/V2 "Vulnerable" at boot), SECCOMP,
  STACKPROTECTOR, MODULE_SIG, SECURITY (no LSMs), FORTIFY_SOURCE,
  HARDENED_USERCOPY, BPF_SYSCALL, KPROBES, IO_URING, USERFAULTFD, COMPAT.
- ON: KASLR (RANDOMIZE_BASE+RELOCATABLE), STRICT_KERNEL_RWX +
  STRICT_MODULE_RWX, X86_KERNEL_IBT, INIT_STACK_ALL_ZERO, VMAP_STACK.
- **Variant MODULES=n** removes the ring-0 module-injection surface
  entirely (no module loading, no hw pack). Default MODULES=y relies on
  the W6 marker gate (landed) + P2 pin contract (pending) for pack trust.
- rcl2: root PID1, no capability separation (single-user tool distro;
  physical access ≈ root by design); console getty spawns root shell
  (no auth). busybox login/su vs missing `/etc/shadow` UNTESTED (mission
  67, queued). udhcpc.script audited CLEAN (DHCP-input charset validation,
  atomic resolv.conf).

---

## 6. Test gates — MEASURED

`make check` **EXIT=0**: **181 unit tests** (17 stability + 14 rcl2
lifecycle + 45 l2pm security + 19 pipeline + 3 validation + 4 features +
33 transactions + 17 l2gpt + 18 l2install + 7 l2net + 4 l2wifi) **+ 5 rcl2
C suites** (selftest/supervision/truncation/lifecycle/tmpcap+tmpcap-real)
+ l2wifi build. Evidence: `build/campaigns/a2-make-check-w6green.log`.
All four implementation workstreams W1/W2/W6/W7 DONE + lead-verified.

Reproducibility: ISO **assembly** byte-reproducible proven (xorriso +
limine + MBR-pin given identical staging; 06a4fd1e twice). **Compile-layer**
double-build proof RUNNING for the variant (isolated `build-lowram/repro`,
L2_BUILD_DIR). Pin every staging input hash; do not assume compile repro.

---

## 7. Release blockers / pending decisions

1. **ORPHANED default-kernel lane (USER DECISION):** L2-72 session gone.
   No executor for mission-05 default-kernel trim iteration, P2 build-side
   (l2 lane), N5 kernel-side threshold finalization, or the FINAL
   integrated `./l2 build` (rcl2 l2pm rootfs initramfs iso). Fragment WIP
   committed clean (no work lost). Options: assign to L2-54, to L2-ea,
   wait for L2-72, or pause. **The integrated default ISO is unblocked on
   the implementation side (W6 green + W7 gate PASS) — only the builder is
   missing.**
2. **Variant FINAL (L2-54 lane, in progress):** WP2 v3 repro double-build →
   attribution + report → re-verification (10M/9M/subset/mempressure) →
   post-W7 rcl2 re-copy + selftest re-stamp → FINAL sha pin. Proceeds
   INDEPENDENTLY of the orphaned lane.
3. **9M stretch probe** (PREDICTED boot; measure in FINAL suite).
4. **l2pm Makefile alignment** (orphaned) → final sizes → README refresh.
5. **N5 joint threshold policy:** kernel-side (orphaned) + l2net boot()
   gate must share one policy and land together; l2net side ready, pauses.
6. **Heavy-test follow-ups (queued, verification tier):** W6 live-boot QEMU
   (real root mounts for F5/F14 positive paths, find_pack ordering fuzz);
   W7 §10 (ctrlaltdel/halt signals, wait-mode under pressure, UEFI boot of
   repacked ISO, fork-bomb/RLIMIT_NPROC, rescue live-verify, 16M
   install-from-live ENOSPC); l2pm concurrency + power-loss (dm-flakey).
7. **README product-doc refresh** (held until final sizes settle).

## 8. Release recommendation (draft)

- **Variant `l2-lowram.iso`:** on track for FINAL after WP2 re-verification
  + sha pin (10M boot MEASURED, idle ~3.8M flat, MODULES=n minimal surface).
  Recommend release once the 9M probe + repro double-build + mempressure
  gate are stamped.
- **Default integrated ISO:** implementation complete and green, but
  release BLOCKED on the orphaned-builder decision (item 1). Do not ship
  until an executor builds + verifies the integrated image with W1/W2/W6/W7.
- **Do NOT claim** 9M boot, compile-layer reproducibility, or P2 pack-pin
  protection until each is measured/implemented respectively.