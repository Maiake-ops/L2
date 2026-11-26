# Phase 00 Verified Baseline — L2: ARCHNEMESIS

Recorded by **XENON** prior to any campaign code edits (read-only repository audit; all temporary directories in `/tmp`).

## 1. Repository & Host Snapshot
- **Branch / HEAD**: `campaigns/armageddon-ii...origin/campaigns/armageddon-ii` @ `380a3651e`
- **Pre-existing dirty tree (untouched)**: `M desktop/external.mk`, `M desktop/rootfs/bin/netsurf`, `M lib/desktop.sh`, `M tools/test-desktop.py`, `?? media/`
- **Host**: NixOS 26.11 (Zokor), `Linux nixos 7.2.13-xanmod1 #1-NixOS SMP PREEMPT_DYNAMIC x86_64`, 16 CPUs (AMD Ryzen 7 7735HS), 12 GiB RAM (~6.6 GiB available), 13 GiB swap
- **Virtualization**:
  - KVM: `/dev/kvm` (`crw-rw-rw- root kvm`), read/write **YES**
  - OVMF: `/home/maiake/.nix-profile/share/qemu/edk2-x86_64-code.fd` (`3,653,632 B`), **YES** (auto-detected by `tools/ramtest.py`)
- **Sandbox note (verbatim)**: Sandboxed `run_command` (`BypassSandbox: false`) failed consistently with:
  `Command failed to run: connecting to sandbox server: read unix @->@: recvmsg: connection reset by peer.`
  All baseline commands ran with `BypassSandbox: true`.
- **Toolchain versions**: `gcc 16.2.0`, `clang 21.1.8`, `ld.lld 21.1.8`, `nasm 3.02`, `rustc 1.99.0`, `cargo 1.99.0`, `python3 3.14.8`, `qemu-system-x86_64 11.1.2`, `xorriso 1.5.8.pl02`, `gum 2.0.2`, `strace`, `unshare`, `sgdisk`, `mkfs.ext4`, `e2fsck`, `mkfs.fat`, `mtools`, `cpio`, `gzip`, `xz`, `depmod`, repo musl wrapper `build/bin/musl-gcc`.
- **Host caveat**: `~/.nix-profile/bin/find` and `date` are BusyBox v1.37.0 applets (no `-ls`, no `%N`).
- **Absent host tools**: system `musl-gcc`, `rustup`, `go`, `ninja`, `cmake`, `mksquashfs`, `shellcheck`, `pytest`, `grub-mkrescue`, `syslinux`, `isohybrid`, `kpartx`.

## 2. Offline Verification Baseline (`make check`, `./l2 doctor`, selftests)
- **`make check`**: **PASS (`rc=0`)**, wall time **21.060s**, **228/228 Python unit tests PASS (0 failures, 0 skips)**:

| Gate / Suite | Count | Duration | Result |
|---|---:|---:|---|
| Shell syntax (`bash -n`/`sh -n` x12) + `py_compile` (1) + `ramtest --selftest` | 14 | <0.2s | PASS |
| `test_ramtest.py` | 10 | 0.008s | PASS |
| `test_lowram_matrix.py` | 39 | 0.003s | PASS |
| `test_lowram_build.py` | 3 | 0.342s | PASS |
| `test_stability_harness.py` | 17 | 0.444s | PASS |
| `make -C rcl2 test` (`--selftest` + 5 C binaries) | 6 | <0.5s | PASS |
| `test_rcl2_lifecycle.py` (incl. 6 userns PID 1 tests) | 14 | 8.968s | PASS |
| `test_l2pm_security.py` | 45 | 0.549s | PASS |
| `test_l2pm_pipeline.py` | 19 | 3.805s | PASS |
| `test_l2pm_recipe_validation.py` | 3 | 0.031s | PASS |
| `test_l2pm_features.py` | 4 | 0.298s | PASS |
| `test_l2pm_transactions.py` (incl. `strace` fault injection) | 35 | 3.307s | PASS |
| `make -C l2pm check` (disposable `L2PM_ROOT`) | 1 | <0.1s | PASS |
| `test_l2gpt.py` | 17 | 0.952s | PASS |
| `test_l2install.py` | 19 | 0.825s | PASS |
| `test_l2net.py` | 7 | 0.233s | PASS |
| `test_l2wifi.py` | 4 | 0.823s | PASS |

- **`./l2 doctor`**: **PASS (`rc=0`)** — 30/30 host tools ok, 15 built components, core ISO `8.2 MiB / 10 MiB`, real-HW essentials present, `build/out/l2pm selftest` `rss: 156 KiB`.
- **`./l2fetch --no-color --no-logo`**: **PASS (`rc=0`, 0.085s)**.
- **`l2pm selftest` (VmHWM from `/proc/self/status`, `l2pm.c:3350`)**:
  - `build/out/l2pm` (`88,184 B`): `sha256: ok`, `reproducible tar: ok`, VmHWM **152 KiB** (156 KiB via `./l2 doctor`, budget 10,240 KiB).
  - `l2pm/l2pm` (`100,472 B`): `sha256: ok`, `reproducible tar: ok`, VmHWM **160–164 KiB** (budget 10,240 KiB).
- **`rcl2 --selftest`**:
  - `build/out/rcl2` (`17,184 B`): resident **48 KiB** (stale: runs only cmdline tests 0..2).
  - `rcl2/build/rcl2` (`15,704 B`): resident **44–48 KiB** (current: runs cmdline tests 0..4).

## 3. Pre-Campaign Artifact Sizes & SHA-256 Inventory

| Artifact | Bytes | SHA-256 | Baseline Notes |
|---|---:|---|---|
| `build/out/l2.iso` | 8,636,416 | `66ddd6aad821297482712fb28b8c8fc1f866ec131caaa104b5ec539aa4d7db97` | 8.24 MiB (<10 MiB gate) |
| `build/out/l2-wifi.iso` | 51,517,440 | `8c2214b0f408c33b72e5c9e073a2f7c1b1d25062414117520d4a420c182cf83c` | 49.13 MiB |
| `build/out/l2-desktop.iso` | 76,361,728 | `2100afe5cbe071d273f643148dc292bfd8a3cda0efbba3b0fa3a0018b9b865a1` | 72.82 MiB |
| `build/out/vmlinuz-l2` | 2,831,360 | `4848e46fdec98de75809600e50f64e6b41b2fe41ec84f58a20a1bedd16dafd98` | `7.3.3-l2` |
| `build/out/initramfs-l2` | 422,838 | `e4ac7d49f44dc40137fc14c661bc8aeb984731c1e10b8191b7c4cbc93659660b` | 412.9 KiB |
| `build/out/initramfs-l2-desktop` | 425,911 | `68c162657f14a37e0ac9813e58baee5a335bb7b648327eae69f236817ec771f2` | 415.9 KiB |
| `build/out/rcl2` | 17,184 | `e6dd58efa1d5d2f1827f0a8d483b960358019b1649ff3052230a42766db962d5` | Stale vs `rcl2/build/rcl2` |
| `rcl2/build/rcl2` | 15,704 | `cc94c07f9f566a21fd2592f118f40f1f834d0904454c7d48183a213ad7b92e3e` | Tested by `make check` |
| `build/out/l2pm` | 88,184 | `efc328191c2d955cf70695116be402288d9ac369cf901af7326fef94ec8d0f81` | Stale; built w/ `--gc-sections` |
| `l2pm/l2pm` | 100,472 | `a3d008922b58ef124a15de578e7143676eacf6dd2829048011fdd2a6d67f62a6` | Tested by `make check` (no `--gc-sections`) |
| `build/out/l2wifi` | 63,416 | `d3f7957f8fbca5a96741cd3d5e53e5d9a600be9f2c52ce651de9496f09cdf841` | Built w/ `--gc-sections` |
| `l2wifi/l2wifi` | 63,416 | `baae3075d4002e7c8fa063b23b27e25c5185171029f7c554764bc749777574ea` | Differs 39,330 B (no `--gc-sections`) |
| `build/out/busybox` | 374,752 | `62451198ce29f9beb695eff8bcc8ec97e92827b13db91bc803bbe574715cfab8` | Matches rootfs |
| `build/out/dash` | 145,168 | `7a5eb7ad977a48669314072baa6378d78117e391aecf2dcc56c8770321bc97a2` | Matches rootfs |
| `build/out/l2gpt` | 42,672 | `e977e0e5932aef26a8e0160b6cb81b1de5a7d9e4d7a99e4181847d40a9c1c3ef` | Static musl |
| `build/out/l2audio` | 63,224 | `801d092ef331279e9063808d76dea5757694782004ca678d24fc842304d92a8d` | Static musl |
| `build-lowram/out/l2-lowram.iso` | 5,961,728 | `01c784a1ce6fca03d36d4090b6f4b20098680972833350c7c70958f351fd0c3d` | Stale vs current `config/cmdline` (rebuild = 5,965,824 B) |
| `build-lowram/out/vmlinuz-l2-lowram` | 1,647,616 | `4d38a2a76248065af70d8f9bd73d5f65f624604a48c24eb0eaea291c20048530` | `7.3.3-l2-lowram` |
| `build-lowram/out/initramfs-l2-lowram` | 420,933 | `a6974f5aba2209eb3ac09ef0676ce7e76e7eff653398adcb44ae9680f28c91ce` | 411.1 KiB |

- **Path-leak audit**: 0 `/home/maiake` or `/nix/store` strings in all 9 stripped core binaries.
- **ISO determinism check**: 2 back-to-back `tools/l2-lowram iso` runs separated by 2s produced byte-identical ISOs (`5,965,824 B`, SHA-256 `9202626f676046711e1f5e89bf026b3832e6bf593955df41134acf863e46fbda`).

## 4. QEMU + KVM + OVMF Boot & Memory Baseline

### 4a. Official `tools/ramtest.py` (`--settle 40`, `-smp 2`, `-nic none`, clean `poweroff`)

| ISO & Firmware | Guest RAM | MemTotal | Idle Used | Kernel Reserved | PID 1 RSS | Result |
|---|---:|---:|---:|---:|---:|---|
| `build/out/l2.iso` BIOS | 256 MiB | 250,516 KiB | **5,596 KiB** | 11,628 KiB | **44 KiB** | PASS (`7.3.3-l2`) |
| `build/out/l2.iso` UEFI | 256 MiB | 244,592 KiB | **5,764 KiB** | 17,552 KiB | **40 KiB** | PASS (`7.3.3-l2`) |
| `build-lowram/out/l2-lowram.iso` BIOS | 10 MiB | 5,440 KiB | **3,828 KiB** | 4,800 KiB | **36 KiB** | PASS (`7.3.3-l2-lowram`) |
| `build-lowram/out/l2-lowram.iso` UEFI | 128 MiB | 118,500 KiB | **3,956 KiB** | 12,572 KiB | **36 KiB** | PASS (`7.3.3-l2-lowram`) |

### 4b. Sequential Boot Latency, Low-RAM Floor & `/proc/meminfo` Breakdown (`settle=5s`, KVM, `-smp 2`)

| Configuration | Prompt | Poweroff | Used / Total (KiB) | Reserved | Slab / Shmem / PT / KStack / Percpu | PID 1 RSS / HWM | Status |
|---|---:|---:|---:|---:|---|---|---|
| Core BIOS 256M | 4.67s | 0.16s | 5,600 / 250,516 | 11,628 | 3368 / 840 / 452 / 408 / 312 | 48 / 48 KiB | PASS |
| Core UEFI 256M | 6.03s | 0.21s | 5,792 / 244,592 | 17,552 | 3424 / 840 / 556 / 400 / 312 | 48 / 48 KiB | PASS |
| Core BIOS 32M | 4.68s | 0.21s | 5,596 / 24,716 | 8,052 | 3364 / 840 / 468 / 408 / 312 | 44 / 44 KiB | PASS |
| Core BIOS 20M | 4.71s | 0.21s | 5,596 / 12,616 | 7,864 | 3360 / 840 / 476 / 408 / 312 | 44 / 44 KiB | PASS |
| Core BIOS 16M | 4.76s | 0.21s | 5,612 / 8,588 | 7,796 | 3356 / 840 / 480 / 408 / 312 | 48 / 48 KiB | PASS |
| Lowram BIOS 10M | 4.14s | 0.16s | 3,832 / 5,440 | 4,800 | 2020 / 828 / 404 / 240 / 32 | 40 / 40 KiB | PASS |
| Lowram BIOS 8M | — | — | — | — | — | — | FAIL (silent halt @ 3.95s) |
| Lowram UEFI 128M | 5.46s | 0.16s | 3,972 / 118,500 | 12,572 | 2040 / 828 / 508 / 240 / 32 | 32 / 32 KiB | PASS |
