# Mission 01 — x86_64 boot process analysis (DONE 2026-11-20)

Evidence: `build/campaigns/boot-forensics-32M.log` (full serial boot, 32 MiB
guest, memblock=debug + initcall_debug), `build/campaigns/armageddon-baseline-sweep.log`,
source reads cited inline. All paths relative to repo root.

## Boot chain, live ISO (BIOS and UEFI)

1. **Firmware → Limine 9.6.7** (pinned in `sources.lock`, built by
   `build_limine()` in `l2:1413`). ISO is a hybrid: BIOS El Torito
   (`limine-bios-cd.bin`, no-emul, load-size 4) + UEFI El Torito
   (`limine-uefi-cd.bin`, stock 2.8 MiB FAT image — OVMF rejects smaller) +
   protective MBR; `limine bios-install` then pins the MBR disk ID to
   `L2\0\1` at offset 440 for reproducibility (`l2:_make_iso`, ~1467–1533).
2. **Limine menu** `config/limine.conf` (copied to `/boot/limine/limine.conf`
   and `/EFI/BOOT/limine.conf`): timeout 3, default entry 1 "/L2 live".
   `protocol: linux`, `kernel_path: boot():/boot/vmlinuz`,
   `module_path: boot():/boot/initramfs`. `@CMDLINE@` is substituted from
   `config/cmdline` at ISO build time.
3. **Kernel command line (live)**:
   `rdinit=/init console=ttyS0,115200 console=tty0 loglevel=4
   dhash_entries=2048 ihash_entries=2048 mhash_entries=128 mphash_entries=128
   thash_entries=128 uhash_entries=128`.
   `console=tty0` last ⇒ /dev/console is the screen; serial still gets logs
   and a getty. Hash-table entries are capped for the RAM budget.
4. **bzImage decompression**: `CONFIG_PHYSICAL_START=0x200000` (2 MiB) keeps
   the decompression window at 2..14 MiB so low-RAM BIOS boots don't overwrite
   an initramfs placed near top-of-RAM (fragment comment, `config/kernel.fragment`).
   `CONFIG_RELOCATABLE=y` + `CONFIG_RANDOMIZE_BASE=y` (KASLR on);
   `CONFIG_PHYSICAL_ALIGN=0x200000`. The RAM patch sets linker
   `max-page-size=0x1000` under BASE_SMALL and relaxes the decompressor's
   2 MiB LOAD-alignment check (`tools/patch-kernel-ram.py`).
5. **Kernel proper**: Linux 7.3.3 (`build/out/kernelrelease` = `7.3.3-l2`),
   tinyconfig + `config/kernel.fragment` (+ optional `config/kernel.local`),
   `make olddefconfig`, then `tools/patch-kernel-ram.py` source patches
   (applied by `build_kernel()` at `l2:594` BEFORE configure; the source tree
   is re-extracted on every kernel build, so manual edits to `build/src` do
   not survive).
6. **initramfs unpack**: gzip newc cpio built by `build_initramfs()`
   (`l2:1395`): `find | LC_ALL=C sort -z | cpio --null -o --format=newc
   --owner=0:0 [--reproducible] | gzip -9 -n`. Under BASE_SMALL the patch
   makes unpack synchronous and frees consumed initrd pages progressively
   during decompression — observed working: repeated
   `Freeing initrd memory: 16K` lines at ~1.03 s in the forensics log.
   The initramfs IS the rootfs (no pivot_root on live); `/init` → `bin/rcl2`.
7. **PID 1 = rcl2** (`rcl2/src/start.s`): `_start` aligns rsp, passes
   argc/argv/envp pointer to `rcl_enter` (C core `src/main.c` + Rust core
   `src/rs/rcl.rs`), no libc, raw syscalls; never returns (exit would kill
   PID 1). Parses `/etc/rcl2.conf` (`config/rcl2.conf`): hostname L2, klog 4,
   ctrlaltdel poweroff; services:
   - `net wait /bin/dash /bin/l2net boot` (blocks until done)
   - `tty1 respawn /sbin/rcl2 console /dev/tty1`
   - `ttyS0 respawn /bin/dash /etc/l2/serial-getty ttyS0` (shell only if UART exists)
   Boot observed: `rcl2 ... booted` → `start net pid 51` → net exit 0 →
   tty1/ttyS0 → `resident 68 KiB (budget 150)` → `boot complete: 3 service(s)`.
8. **l2net boot** (`overlay/bin/l2net`): sets `watermark_scale_factor=0`
   (allowed by the kernel patch), `min_free_kbytes=128`,
   `lowmem_reserve_ratio=0 0 0`, `percpu_pagelist_high_fraction=100000`;
   brings up lo; finds the boot-medium pack (`/lib/modules/$KREL` on installed
   root, else mounts ISO9660 read-only at /media/l2 and symlinks
   `l2/hw` or `l2/wifi` modules + firmware path); loads modules ONLY by
   modalias for present hardware (two passes: host buses, then child buses);
   DHCP only on wired interfaces with carrier; wpa_supplicant only if a
   network is saved; finishes with drop_caches + PCP drain + stat_refresh.
9. **Shell**: dash `/bin/sh`, PS1 `L2# ` from `/etc/profile`; motd printed.
   `poweroff/reboot/halt` are wrappers (`overlay/bin/shutdown`) signaling
   rcl2 for clean shutdown.

## Installed-disk path (differs)

`config/limine-installed.conf`: normal entry has **no module_path** — kernel
mounts ext4 root itself via `root=PARTUUID=@ROOT_PARTUUID@ rootwait rw
rootfstype=ext4 init=/sbin/rcl2` (all storage + ext4 built in). Kernel,
recovery initramfs and limine.conf live on the FAT32 ESP (Limine reads only
FAT32/ISO9660). Recovery entries: disk recovery shell, RAM-only rescue
(initramfs, doesn't touch disk). Live ISO also has recovery + `mem=1G` +
`mem=256M` cap entries.

## Observations / defects noted (feed later missions)

- **Low-RAM floor**: 10M and 12M guests FAIL (12M: early OOM panic,
  "System is deadlocked on memory"; DMA32 present 11768 KiB, managed 3212 KiB).
  16M+ PASS. Root cause quantified in mission 03/04 report: ~8 MiB fixed
  reservation floor dominated by the ~7.3 MiB decompressed kernel image.
- **l2net medium probe edge case**: with a block device present but no L2
  medium (e.g. direct-kernel QEMU boot with empty DVD drive), `find_pack`
  retries mount 10×1 s (`/dev/sr0: Can't open blockdev` ×10 in forensics log,
  ~12 s). Real ISO boots mount on first try; installed systems short-circuit
  on /lib/modules. Candidate micro-optimization (mission 79): skip devices
  with no media.
- **TSC unstable under QEMU -smp 2**: "Marking TSC unstable due to
  clocksource watchdog" → switches to hpet (forensics log ~1.32 s). QEMU
  artifact; relevant to mission 82 (CPU models) and boot-latency measurements.
- **CPU mitigations off**: `CPU_MITIGATIONS is not set`; boot prints
  "Spectre V2 : Vulnerable", "Spectre V1 : Vulnerable". Security posture
  decision for mission 61.
- **printk ring too small for debug boots**: 16 KiB ring
  (LOG_BUF_SHIFT=13) overflows before ttyS0 enables at 0.046 s, dropping the
  earliest memblock messages. Forensics must use `earlycon` (mission 16).
- rcl2 resident at boot: 68 KiB (selftest: 48 KiB), budget 150 KiB — healthy.

## Limitations

- Forensics log used direct `-kernel/-initrd` boot (fw_cfg initrd placement,
  ~1 MiB higher RAM readings than ISO); reservation *composition* is still
  representative, absolute budget numbers come from the ISO sweep only.
- Early memblock dump partially lost to ring overflow; re-capture with
  earlycon planned under mission 03/16.