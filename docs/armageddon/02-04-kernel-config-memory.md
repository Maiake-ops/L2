# Missions 02/03/04 — kernel config audit + early-boot memory forensics (2026-11-20)

Evidence: `build/out/kernel.config` (built 14:39), symbol-state extract
`build/campaigns/02-config-audit-symbols.txt` (243 lines),
`build/campaigns/boot-forensics-32M.log`, `build/campaigns/armageddon-baseline-sweep.log`,
`size`/`nm` on `build/src/linux-7.3.3/vmlinux`.

## Mission 02 — configuration audit

Base: `make tinyconfig` + `config/kernel.fragment` + `make olddefconfig`
(Linux 7.3.3). Fragment intent: real-HW boot support, flat idle RAM, wifi as
modules, small image. Verified posture (full list in the symbols file):

**On, as intended**: 64BIT/SMP/NR_CPUS=4, HZ_100/NO_HZ_IDLE/HIGH_RES_TIMERS,
PREEMPT_LAZY, EXPERT, SLUB+SLUB_TINY+SLAB_MERGE_DEFAULT, BASE_SMALL,
LOG_BUF_SHIFT=13, SPARSEMEM, PCP_BATCH_SCALE_MAX=0, CC_OPTIMIZE_FOR_SIZE,
DEBUG_INFO_NONE, KALLSYMS=n, BLK_DEV_INITRD+RD_GZIP only, DEVTMPFS(no auto-mount),
PROC/SYSFS/TMPFS/SHMEM, BINFMT_ELF+SCRIPT, MULTIUSER, FUTEX/EPOLL/SIGNALFD/
TIMERFD/EVENTFD/POSIX_TIMERS/FILE_LOCKING, UNIX98_PTYS, SERIAL_8250 2 UARTs,
VT+FB_VESA+FB_EFI+FRAMEBUFFER_CONSOLE+FONT_8x16+FONT_TER16x32, storage built-in
(SCSI/SD/SR/ATA_PIIX/AHCI/NVMe/MMC_SDHCI/USB_XHCI+EHCI+STORAGE/VIRTIO_BLK),
EXT4(+ext2)/VFAT/MSDOS/ISO9660+JOLIET/NLS, I2C core+GPIOLIB+PINCTRL_AMD=y,
HID_GENERIC+USB_HID+ATKBD+PS2 mouse, ACPI (BUTTON/AC/BATTERY/PROCESSOR/
SUSPEND/PM/EFI/EFI_STUB/EFIVAR_FS), RTC_CMOS, INET+PACKET+UNIX+VIRTIO_NET,
MODULES+UNLOAD (no SIG/COMPRESS), FW_LOADER+XZ/ZSTD compress, MAGIC_SYSRQ,
STRICT_KERNEL_RWX+STRICT_MODULE_RWX, X86_KERNEL_IBT, INIT_STACK_ALL_ZERO,
RELOCATABLE+RANDOMIZE_BASE, PHYSICAL_START=0x200000.

**Modules (hw/wifi packs, zero idle cost)**: all wifi (cfg80211/mac80211 +
Intel/MediaTek/Realtek/Atheros/Broadcom), physical NICs (e1000/e1000e/igc/
r8169/usb-net), OHCI/UHCI/UAS, I2C_DESIGNWARE/I2C_HID/HID_MULTITOUCH,
EXFAT, LEDS_CLASS, BACKLIGHT, ACPI_VIDEO/WMI/FAN/THERMAL, X86_PKG_TEMP_THERMAL.

**Off, good for RAM/size**: CGROUPS (⇒ no memcg; mission 28 resource controls
must use RLIMITs, not cgroups), NUMA, THP, KSM, COMPACTION, SWAP,
DEFERRED_STRUCT_PAGE_INIT, SCHED_CACHE/CLUSTER, AIO, IO_URING, ADVISE_SYSCALLS,
USERFAULTFD, PROC_PAGE_MONITOR, IPV6, NETFILTER, BRIDGE/TUN/VETH/MACVLAN,
INET_DIAG/PACKET_DIAG/UNIX_DIAG/XFRM_USER, IP_ADVANCED_ROUTER, TCP_CONG_ADVANCED,
NFS/CIFS/9P/SUNRPC, KEXEC/CRASH_DUMP, MEMORY_HOTPLUG/HOTREMOVE, HUGETLBFS?,
SYSVIPC, POSIX_MQUEUE, AUDIT, IKCONFIG, KPROBES, JUMP_LABEL, FTRACE/TRACING,
DEBUG_FS?, BPF_SYSCALL, SECCOMP, STACKPROTECTOR, MODULE_SIG, COREDUMP,
BINFMT_MISC, IA32_EMULATION, COMPAT, X86_5LEVEL?, X86_MCE, X86_MSR/CPUID,
X86_FRED, X86_USER_SHADOW_STACK, MODIFY_LDT_SYSCALL, X86_BUS_LOCK_DETECT,
EFI_MIXED, EFI_CAPSULE_LOADER, ACPI_APEI/NFIT/DOCK/TAD/PCI_SLOT/SBS/HED/BGRT/
CONFIGFS/FPDT/EC_DEBUGFS/PROCESSOR_AGGREGATOR, DRM, SOUND, MEDIA_SUPPORT,
FW_LOADER_USER_HELPER, CONNECTOR/PROC_EVENTS/TASKSTATS/BSD_PROCESS_ACCT,
IRQ_TIME_ACCOUNTING/VIRT_CPU_ACCOUNTING_GEN, SCHED_CORE/AUTOGROUP,
RCU_EXPERT, CPU_MITIGATIONS (all MITIGATION_* off).

**On, NOT requested by the fragment — trim candidates (mission 05)**:
- `PERF_EVENTS=y` (+ Intel PMU core; `intel_pmu_init` 19 KiB single symbol).
  L2 userland has no perf consumer. Fragment only disabled UNCORE/RAPL/CSTATE.
- `CPU_FREQ=y` + `CPU_FREQ_GOV_PERFORMANCE=y` + `CPU_FREQ_GOV_SCHEDUTIL=y` +
  `CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y` + `X86_INTEL_PSTATE=y` + `X86_AMD_PSTATE=y`
  — pulled in by `ACPI_PROCESSOR=y` (Kconfig select). Substantial code;
  evaluate making the P-state drivers modular/off vs real-HW thermal needs.
- `THERMAL=y` + `THERMAL_DEFAULT_GOV_STEP_WISE=y` + `X86_THERMAL_VECTOR=y`
  (selected by ACPI_THERMAL=m / X86_PKG_TEMP_THERMAL=m).
- `IOSF_MBI=y` (Intel SoC sideband; selector needs checking).
- `PINCTRL_AMD=y` (fragment explicit; AMD SoC pinctrl — candidate for =m in
  hw pack, it serves I2C-HID interrupt routing on AMD laptops).
- ACPI extras: `ACPI_CONTAINER=y`, `ACPI_HOTPLUG_IOAPIC=y`, `ACPI_HOTPLUG_CPU=y`,
  `ACPI_MRRM=y`, `ACPI_LPIT=y`, `ACPI_SPCR_TABLE=y`,
  `ACPI_REV_OVERRIDE_POSSIBLE=y`, `ACPI_PROCESSOR_IDLE=y`.
- `DEBUG_KERNEL=y` (selected by EXPERT) — check which DEBUG_* defaults it
  pulls (DEBUG_LIST/SG/NOTIFIERS etc.); `__bug_table` is 57 KiB (BUG=y, kept).
- `X86_VSYSCALL_EMULATION=y` (legacy; musl/dash/rcl2 don't need it).
- `INIT_STACK_ALL_ZERO=y` — security hardening with runtime cost; keep unless
  mission 80 shows it matters (document trade-off, mission 61).
- `VIDEO=y` is only the graphics menu helper (APERTURE_HELPERS/SCREEN_INFO) —
  NOT a trim target (verified context at kernel.config:2673).
- `MICROCODE=y` — keep (real-HW errata fixes; small).

**Security posture notes (mission 61 input)**: CPU_MITIGATIONS off (boot log:
"Spectre V2/V1: Vulnerable"), SECCOMP off, STACKPROTECTOR off, X86_MCE off,
MODULE_SIG off, no LSMs, CGROUPS off. HARDENED_USERCOPY/FORTIFY/SMAP/SMEP/PTI
states: see symbols file. This is a deliberate tiny-distro trade-off surface;
mission 61 must document it and decide what to enable.

## Mission 03 — early-boot memory profile (32 MiB guest, forensics log)

`Memory: 22252K/32248K available (4891K kernel code, 351K rwdata,
1392K rodata, 692K init, 164K bss, 9060K reserved, 0K cma-reserved)`

- vmlinux (uncompressed ELF): text 5,626,568 + data 1,928,616 + bss 299,008
  = 7,854,192 B (~7.5 MiB). bzImage 2,958,336 B. Persistent image after init
  freed ≈ 6.8 MiB. **The image is the dominant reservation.**
- percpu: 2 MiB allocated (1 MiB unit × 2 CPUs), freed back to 39 pages/cpu
  (~156 KiB/cpu actual) — `percpu: Embedded 39 pages/cpu s121176 r8192 d30376
  u1048576`. Patch's early page-table cap visible: `early_dynamic_pgts` 64 KiB
  (16×4K, patched from 64).
- printk ring: 8192 data + 28672 meta = 36864 B (LOG_BUF_SHIFT=13).
- Hash tables (cmdline-capped): dentry 4096 entries/32 KiB, inode 2048/16 KiB,
  mount-cache 512/4 KiB, mountpoint 512/4 KiB, posixtimers 512/8 KiB,
  futex 16/1 KiB. (TCP/UDP caps apply at net init.)
- `mem auto-init: stack:all(zero), heap alloc:off, heap free:off`.
- SLUB: HWalign=64, Order=0-1, MinObjects=0 (SLUB_TINY working).
- NR_IRQS 4352, nr_irqs 440, preallocated 16.
- initrd streaming free works: `Freeing initrd memory: 16K` repeatedly during
  unpack (~1.03 s), initramfs 426,833 B gzip.
- No SWIOTLB reservation line observed in the captured window (early messages
  lost to ring overflow); SWIOTLB_DEFAULT_SIZE_MB=4 + DYNAMIC=y configured.
  Re-capture with earlycon to confirm whether the 4 MiB default is reserved
  at boot on small guests (mission 04 follow-up).
- Largest single symbols (nm --size-sort tail) are all <64 KiB — no single
  monster; size is spread across many built-in subsystems. Subsystem-level
  aggregation is the next step (mission 05).

## Mission 04 — physical reservation / low-RAM failure analysis

ISO sweep (BIOS, -smp 2, settle 40, budget 15360 KiB), current artifacts:

| guest | result | idle used | MemTotal |
|---|---|---|---|
| 10M | FAIL — no prompt | — | — |
| 12M | FAIL — early OOM panic | — | present 11768K, managed 3212K |
| 16M | PASS | 5564 K | 8192 K |
| 24M | PASS | 5632 K | 16256 K |
| 32M…1G | (sweep completing; see log) | | |

- 12M panic at 0.375 s: "Out of memory and no killable processes... System is
  deadlocked on memory". 2139 pages reserved of 2942 present (~8.5 MiB).
  shmem 280K ⇒ initramfs partially unpacked; fails during early initcalls.
- Fixed reservation floor ≈ 8–8.5 MiB, dominated by the kernel image
  (~7.3 MiB) + initrd 417 KiB + memmap + page tables + percpu.
- **Consequence**: guests ≤12 MiB cannot boot this kernel. 16 MiB works with
  ~2.6 MiB managed headroom and excellent idle RAM (5.5 MiB ≪ 15 MiB budget).
- Idle RAM is FLAT across guest sizes (5564→5632 K from 16M→24M): the
  fragment + cmdline + l2net tuning achieve the "no growth with RAM" goal.
  struct-page array (~1.5% of RAM) remains invisible to `free` (physics).
- To boot 10–12 MiB guests the floor must drop ~3–4 MiB — that requires
  either aggressive image reduction (mission 05, safe subset) or an
  architectural change (modular storage + module-loading initramfs, or a
  second "micro" kernel config). Record as decision point; do NOT break
  real-HW built-in storage boot for the installed-disk path.

## Regression assessment

No source changes made by these missions (analysis only). Baseline artifacts
preserved as `build/campaigns/baseline-l2.iso` for before/after comparison.

## Limitations

- Early memblock dump incomplete (ring overflow before console enable);
  earlycon re-capture pending (missions 03/16).
- Direct-kernel forensics inflates absolute RAM ~1 MiB vs ISO (fw_cfg);
  composition conclusions unaffected, budget numbers use ISO sweep only.
- 32M–1G sweep rows still completing at time of writing; ledger updated when
  SWEEP_DONE lands.