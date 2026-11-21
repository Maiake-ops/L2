# L2 ARMAGEDDON II — Release Readiness (W5, missions 281–300)

Status: **DRAFT — variant lane VERIFIED; default integrated ISO pending builder** · Author: L2-ea / L2-aa (lead) · 2026-11-21
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
| Idle RAM (MEASURED) | ~5.5 MiB (16M used 5564K, 24M 5632K) | **3.72–3.74 MiB FLAT** across 9–256M (used 3808–3832K) |
| BIOS boot floor (MEASURED) | **14M** (13M OOM-deadlock) | **9M** (8M FAIL / 9M PASS bracketed; under-10M goal EXCEEDED; v1/v2 10M FAIL) |
| UEFI boot floor (MEASURED) | (96M,128M] — Limine `init_memmap` panic below | 128M PASS, 256M PASS; 16M FAIL-silent (firmware-bound) |
| Kernel image | 7490 KiB decompressed (4891 code/351 rwdata/1392 rodata/692 init/164 bss); bzImage 2,958,336 B | vmlinux **4,494,448 B (4.29 MiB)**, bzImage 1,647,616 B (compile-repro VERIFIED); strips EXT4/INET/NIC/SMP/VT + BUG/BLK_DEV_SD/ACPI_BUTTON/INPUT=n, SWIOTLB=1M |
| Module-injection surface | present → W6 marker gate + P2 pin (pending) | **removed** (MODULES=n, no pack) |
| Core ISO size (MEASURED) | 8,787,968 B (8.38 MiB < 10 MiB gate) | variant FINAL ISO **5,961,728 B (5.69 MiB)**, sha `01c784a1…` **VERIFIED** |

**Headline:** the variant boots at **9 MiB (strictly under 10 MiB)** total
guest RAM with ~3.8 MiB idle — flat across guest sizes — **exceeding** the
campaign's primary low-RAM boot target. The default kernel retains
real-hardware breadth at a 14M BIOS floor. UEFI low-RAM is bootloader-bound
(Limine), not kernel-bound, for both.

> **⚠ VARIANT FUNCTIONAL TRADE-OFF (PENDING USER DECISION):** the actual
> v3 kernel that boots at 10M achieves it by **stripping EXT4, INET, NIC
> drivers, SMP, and VT** (L2-54 preservation/attribution correction: the
> committed `config/kernel.lowram` fragment was an *unbuilt functional
> draft*; the built/tested v3 differs). **Attribution PROVEN from the
> build-time `.config`** (`build-lowram/v3/kernel-lowram.config`):
> `SMP/INET/VT/EXT4_FS` not set, `VIRTIO_NET/NETDEVICES` absent;
> `EFI_STUB/ACPI/PACKET/UNIX/VIRTIO_BLK/SERIAL_8250_CONSOLE/ISO9660` kept
> (boots from ISO + serial console, but no IP networking, ext4, SMP, VT).
> The variant is therefore a
> **minimal-boot configuration, NOT a general-purpose kernel** — no ext4
> disk install, no networking, no SMP, no virtual terminal. The 10M boot
> is real (MEASURED) but comes at this functional cost — **VERIFIED FACT**:
> no ext4 → no disk-install from the variant; no INET/NIC → no networking;
> 1 CPU (SMP off); serial-only (VT off). **USER posture decision (3
> options):** (a) extreme 10M shell (v3, stripped), (b) functional 12M
> (v1, retains INET+EXT4+SMP+VT, idle ~5.0M), or (c) ship both. Do not
> present the variant as a drop-in replacement.

**Version attribution (WP2 FINAL, corrected):** v1 = INET+EXT4+SMP+VT
retained (12M PASS, used 5032K, idle ~5.0M); v2 = dropped INET, kept
EXT4; v3 = dropped EXT4+SMP+VT on top (10M PASS). The "functional 12M"
alternative is **v1, NOT v2** (earlier v1/v2 conflation corrected).
**Preservation (RESOLVED — committed `9da900ab2`):** the earlier committed
HEAD `config/kernel.lowram` was an *unbuilt v4 draft* (restores INET/EXT4;
never built — v4 build interrupted at extraction, bulk commit captured the
draft). L2-54 commit `9da900ab2` "fix(lowram): preserve true built v3
fragment; add rcl2 override to variant builder" restores commit == built
artifact (`config/kernel.lowram` + `tools/l2-lowram`, 32+/16−); the draft
is preserved at `build-lowram/v3/kernel.lowram.functional-draft-NOT-BUILT`.
Posture remains a USER decision (evidence preservation, not shipping).
Authoritative built v3 config also at `build-lowram/v3/kernel-lowram.config`.

### <10M stretch — MEASURED: 9M BIOS PASS (goal EXCEEDED)
**9M BIOS PASS** (L2-54 independent parent measurement, refreshed post-W7
ISO): MemTotal **4416K** (capacity model predicted ~4416K — measured
EXACTLY), used 3812K, reserved 4800K, available 604K (predicted ~580K),
spawn ok, 53.2 s. The variant boots at **STRICTLY UNDER 10 MiB** total
guest RAM — the campaign stretch goal is **EXCEEDED**, not just met.
**FULL SUITE COMPLETE — BIOS 6/6 PASS, MATRIX_EXIT=0** (committed harness,
refreshed ISO): 9M 4416/3812/4800, 10M 5440/3812/4800, 12M 7488/3808/4800,
16M 11520/3812/4864, 32M 27648/3832/5120, 256M 253448/3820/8696; idle flat
3.72–3.74 MiB. **MEMPRESSURE_EXIT=0** (unedited worktree gate, 16M): PID1
VmRSS 36 kB, NO OOM trace — dd hit ENOSPC at the /tmp cap (W7 caps converted
OOM-kill → clean capacity limit, as designed), reclaim +40K, survived. The
WEDGED→PASS transition is measured on the VARIANT too. **8M MEASURED FAIL**
(silent, 4.1 s, QEMU exit before prompt) → **floor bracketed 8M FAIL / 9M
PASS** (mechanism uninvestigated; optional TCG follow-up).

---

## 2. PID 1 (rcl2) — MEASURED

| Metric | Value | Source |
|---|---|---|
| Binary size | **15,568 B** (sha256 `07fb2c69…3a3b`) | post-W7 build |
| Selftest RSS | 44–48 KiB (budget 150; ±1 page variance) | dual-measured: L2-ea 44 / L2-54 48, same binary `07fb2c69` |
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
- **⚠ VARIANT v3 SECURITY POSTURE — VM-ONLY THROWAWAY GUEST:** the ON-list
  above is the DEFAULT kernel. Variant v3 additionally has **no KASLR, no
  IBT, no PTI, no retpoline, CPU_MITIGATIONS=n, BUG=n** → a VM-only
  throwaway guest configuration. **NEVER run the variant on bare metal or
  untrusted hosts.** (MICROCODE=y + PERF_EVENTS=y remain forced by
  def_bool/select — not disableable without CPU_SUP_*=n / arch change.)
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
limine + MBR-pin given identical staging; 06a4fd1e twice). **Variant
kernel compile-layer double-build VERIFIED PASS** (L2-54): repro bzImage
== v3 byte-identical, sha256 `4d38a2a76248065af70d8f9bd73d5f65f624604a48c24eb0eaea291c20048530`
(1,647,616 B), built in isolated `build-lowram/repro` (L2_BUILD_DIR) from
the working fragment. **Default-kernel + l2pm compile-layer repro still
PENDING** (maiake-f6 campaign 7, orphaned). **rcl2 compile-repro VERIFIED**
(L2-54 re-stamp: committed-source rebuild == post-W7 sha `07fb2c69…`
exactly, selftest ok 44 KiB). Pin every staging input hash.

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
2. **Variant FINAL (L2-54 lane): COMPLETE + VERIFIED.** BIOS 6/6 PASS
   (MATRIX_EXIT=0), mempressure PASS (MEMPRESSURE_EXIT=0, clean ENOSPC),
   8M FAIL/9M PASS bracket, repro double-build PASS, sha pin verified.
   Evidence: `build/campaigns/lowram-final-verify/` (+ `-8m/`). Config
   frozen pending USER posture decision.
3. **9M stretch: MEASURED PASS** (goal exceeded); 8M MEASURED FAIL — floor
   bracketed. (8M failure mechanism uninvestigated; optional TCG follow-up.)
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

- **Variant `l2-lowram.iso`:** **VERIFIED** — BIOS 6/6 PASS (9M floor, 8M
  FAIL bracketed), mempressure PASS (W7 caps → clean ENOSPC, no OOM),
  reproducible, sha pin verified: ISO `01c784a1…` (5,961,728 B), kernel
  `4d38a2a7…` (1,647,616 B), initramfs `a6974f5a…` (420,933 B). BUT v3
  STRIPS EXT4/INET/NIC/SMP/VT + hardening (VM-only throwaway, not
  general-purpose). Release now requires ONLY **USER acceptance of the
  functional + security trade-off** (posture: extreme 9–10M shell vs
  functional 12M v1 vs both). Do not ship as a general kernel.
- **Default integrated ISO:** implementation complete and green, but
  release BLOCKED on the orphaned-builder decision (item 1). Do not ship
  until an executor builds + verifies the integrated image with W1/W2/W6/W7.
- **Do NOT claim** default-kernel/l2pm compile-layer reproducibility or P2
  pack-pin protection until measured/implemented. (9M boot, variant-kernel
  + rcl2 compile-repro are now MEASURED/VERIFIED; 8M floor bracket pending.)