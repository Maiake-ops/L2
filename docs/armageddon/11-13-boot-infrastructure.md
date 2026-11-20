# Missions 11/12/13 — bootloader audit + initramfs placement (DONE 2026-11-20)

Evidence: `l2:build_limine` (~1413–1462), `l2:_make_iso` (~1467–1533),
`config/limine.conf`, `config/limine-installed.conf`,
`build/campaigns/baseline-uefi-forensics.log` (UEFI PASS),
`build/campaigns/boot-forensics-earlycon-32M.log` (full memblock history from t=0).

## Mission 11 — BIOS bootloader (Limine 9.6.7)

- Built from the pinned release tarball (no autotools needed); configure
  `--enable-bios --enable-bios-cd --enable-uefi-x86-64 --enable-uefi-cd`;
  requires nasm + ld.lld + llvm-objcopy + mtools (in `l2 deps`).
- Reproducibility: the host tool seeds the MBR disk ID with `time(NULL)`;
  L2 patches it to `srand(SOURCE_DATE_EPOCH)` and additionally pins the four
  ID bytes to `L2\0\1` at offset 440 after `limine bios-install` — rebuilds
  are byte-identical (README claim; spot-check pending under mission 52).
- ISO: El Torito no-emul boot, `-boot-load-size 4`, `--protective-msdos-label`,
  boot-info-table. Stage 2 (`limine-bios.sys`) lives in `/boot/limine/`.
- Installed disk: stage 1 in MBR, stage 2 in the 1 MiB BIOS stage-2 partition
  (hybrid GPT laid out by l2install/l2gpt). Config addresses root by
  `guid(PARTUUID)` so one file works under BIOS and UEFI.
- Audit findings: no custom bootloader code is maintained in-tree (Limine is
  upstream, pinned + hashed in sources.lock) — attack surface is the pinned
  release; Secure Boot must be off (Limine unsigned, documented). The
  `srand` patch is a local sed on the tarball each build (idempotent, exact
  match). BIOS boot verified at 16M–1G (baseline sweep).

## Mission 12 — UEFI bootloader

- Same Limine build provides `BOOTX64.EFI` + `limine-uefi-cd.bin`. The EFI
  El Torito image MUST be Limine's stock 2.8 MiB FAT image — OVMF rejects
  smaller/reformatted images (documented constraint in `_make_iso`). This
  fixed 2.8 MiB is a major reason the core ISO cannot also carry OpenSSL
  (~3 MiB) under the 10 MiB gate; ssl rides on the wifi ISO / medium packs.
- `limine.conf` is duplicated to `/EFI/BOOT/limine.conf` (Limine searches
  there on UEFI). Kernel+initramfs read via `boot():` (the ESP/ISO volume).
- **Verified today**: `ramtest.py --iso baseline-l2.iso --uefi --ram 256M
  --settle 40` → PASS UEFI, idle 5760 KiB of 244196 KiB (BIOS at 256M:
  5548 KiB — UEFI costs ~212 KiB more, EFI runtime services). OVMF:
  ~/.nix-profile/share/qemu/edk2-x86_64-code.fd (auto-found by ramtest).
- Installed-disk UEFI: `/EFI/BOOT/BOOTX64.EFI` on the 256 MiB FAT32 ESP;
  kernel/initramfs/limine.conf on the ESP (Limine reads only FAT32/ISO9660);
  ext4 root mounted by PARTUUID, no initramfs on the normal entry.
- efivarfs built in (CONFIG_EFIVAR_FS=y) for userspace UEFI variable access;
  EFI_ESRT/EFI_EARLYCON/EFI_RUNTIME_WRAPPERS on; EFI_MIXED off (x86_64-only
  firmware assumed — document as compatibility assumption, mission 84).

## Mission 13 — initramfs memory placement

- Build: sorted newc cpio, `--owner=0:0`, `gzip -9 -n`, epoch mtimes
  (`build_initramfs`, l2:1395). 426,833 B current. Kernel supports RD_GZIP
  only (RD_XZ/BZIP2/LZMA/LZO/LZ4/ZSTD=n) — compression compatibility is
  gzip-only by design (mission 15: any other format fails decompression;
  the ISO gate and boot tests enforce gzip).
- Placement: Limine loads the initramfs per its linux protocol; KASLR
  `mem_avoid_init` keeps the relocated kernel away from it.
  `CONFIG_PHYSICAL_START=0x200000` keeps the decompression window at
  2..14 MiB so low-RAM BIOS boots don't overwrite an initramfs near
  top-of-RAM (fragment comment). At 12M the failure is OOM, not overlap —
  placement itself works.
- **Streaming reclaim verified** (earlycon log): `Freeing initrd memory:
  16K` repeated ~26× during unpack (~416 KiB total) — the patch-kernel-ram
  progressive free works; unpack is synchronous under BASE_SMALL. After
  unpack the initramfs pages are gone; rootfs files live in shmem/pagecache
  (counted in `used`, ~1 MiB).
- Installed-disk normal boot: NO initramfs at all (kernel mounts ext4
  directly) — zero initramfs RAM cost; recovery entries keep one on the ESP.
- earlycon forensics also resolved the mission-04 SWIOTLB question: **no
  swiotlb reservation on ≤32M guests** (all RAM below 4G, no IOMMU; no
  "software IO TLB" line, no swiotlb region in the memblock dump). The 4 MiB
  SWIOTLB_DEFAULT_SIZE_MB is not part of the small-guest boot floor.
- Top early reservations (32M, earlycon): 2×2048K percpu unit allocations
  (0x1200000/0x1400000, mostly freed back to ~39 pages/cpu), 64K
  early_dynamic_pgts (patched 16×4K), 32K printk ring, 28K low-memory
  trampoline area. The ~7.3 MiB kernel-image reserve happens in head_64
  before `memblock=debug` parses, so it is absent from the dump but included
  in `Memory: ... 9052K reserved`. e820 at 32M: 639K low RAM + 31.875M
  above 1M, 128K ACPI/NVS reserved at 0x1fe0000.

## Regression assessment / limitations

- Analysis + tests only; no source changes. Baseline artifacts preserved
  (build/campaigns/baseline-{l2.iso,vmlinuz,initramfs,kernel.config}).
- UEFI tested at 256M only so far; UEFI low-RAM (16–32M) and OVMF-on-real-
  firmware differences remain for mission 84. The parallel session's
  tools/lowram_matrix.py (untracked, not ours) covers BIOS/UEFI matrices —
  coordinate before duplicating (see ledger).
- Reproducibility byte-identical rebuild claim not yet re-verified this
  session (mission 52 owns it; heavy-coder does a spot-check under 05).