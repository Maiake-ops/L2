# Astra engineering batch 1 · 2026-11-21

The batch started at `7388d77c7` on `campaigns/armageddon-ii`. W6 and the full check target passed before edits. The actionable failures were in the host-side verification path. Ten engineering commits add two missing build gates and fix eight ways the QEMU tool could mismeasure a guest, accept invalid input, or return a misleading result. No runtime binary, kernel fragment or boot image was changed by these commits.

## Restoration and ownership

Five named records were available in this checkout. The reads covered `note/qwen1.txt`, `note/qwen3.txt`, `note/qwen4.txt`, `docs/armageddon-ii-progress.md` and `docs/armageddon-progress.md` (283,323 bytes in total). `note/qwen2.txt` was missing. The primary ledger still has early checkboxes that disagree with its later log. W6 landed in `ac62c0101`. Its earlier failed check and the later passing check describe different source states.

Main was merged at `1910fe1e7`. The checkout already contained 18 additional local commits. The two backup branches were ancestors. The separate `campaigns/engineering` worktree at `7704a3a3c` was unmerged and had outstanding package, Wi-Fi and verification edits. Two Qwen CLI processes had this checkout as their working directory (PIDs 3087231 and 3102286). Process presence did not identify a current assignment. No concurrent build was observed during the initial inspection. The scope here was the top-level check target, host RAM harness, new host tests and this ledger.

The primary working tree already had modified low-RAM artifacts, a v3 snapshot fragment, handoff notes, and untracked results/proposals. Those stayed with their owners. HEAD's `l2` still dispatches the default build to the hardware pipeline. The recorded proposal for changing that dispatch is separate from the v3 evidence. No integration build, remote operation or update to either historical ledger was performed.

## Ten verified missions

A01–A10 are batch IDs. The last column maps each change to an existing campaign objective. The ten commits can be reviewed independently.

| ID | Before → after | Files | Regression evidence | Commit | Objective |
|---|---|---|---|---|---|
| A01 | Top-level check skipped low-RAM matrix regressions → runs 39 offline cases including framing and failure classification | Makefile | Check recipe assertion failed before, passed after. Matrix suite passed. | `a68c29163` | 20, 60 |
| A02 | Package build ran without its component check → SHA-256 self-test, deterministic archive test and binary-size limit run under a temporary package root | Makefile | Recipe assertion failed before, passed after. Component check exited 0. | `83baa1d0f` | 49, 60 |
| A03 | Empty sweep could succeed without booting and malformed lists could start earlier VMs → validate all sizes before launching | tools/ramtest.py, tools/test_ramtest.py, Makefile | Empty, trailing comma, zero and mixed-invalid fixtures. No VM launch on rejection. | `f3e7af448` | 20, 93 |
| A04 | Fallback counted tmpfs in Cached as available and eagerly accessed MemFree → discount Shmem, include buffers and reclaimable slab, use MemAvailable directly when present | tools/ramtest.py, tools/test_ramtest.py | Fallback fixture yields 75 KiB used. Available-only fixture yields 30 KiB. | `ed5783a6a` | 72, 94 |
| A05 | A missing sample or budget overrun could return success in medium mode → both return failure | same two files | Missing sample and 1,000 KiB used against a 100 KiB limit. | `39bbf9b95` | 96 |
| A06 | Shell probe could reuse the memory probe's completion marker or ignore timeout → clear the receive buffer and require fresh completion | same two files | Previous marker with silent guest fails. Real ISO shell probe passed. | `473e8dd99` | 20, 97 |
| A07 | Disk check printed mount output but accepted a live root → require installed marker and device-backed ext4 root in fresh guest output | same two files | Live-root fixture fails, installed-root fixture passes. Freshly installed virtio disk reported /dev/root on /. | `c34d9cafe` | 38, 86 |
| A08 | Test sent poweroff then killed QEMU in cleanup → allow guest shutdown and require QEMU exit 0 within timeout | same two files | Timeout and nonzero exits fail. Real disk, BIOS ISO and UEFI ISO exited cleanly. | `fc2f82132` | 88 |
| A09 | Conflicting inputs silently selected a different boot path → reject ISO+disk, disk+kernel and half a kernel/initrd pair | same two files | Three CLI cases exit 2. ISO+kernel+initrd remains the existing ISO sweep invocation. | `9d58d575d` | 20, 94 |
| A10 | Medium and sweep defaults sampled at 25 seconds → align both with the established 40-second protocol, preserving explicit overrides | same two files | Parsed defaults and override tested. Real ISO and disk samples used the default. | `8e6c430ef` | 72, 94 |

Local command transcripts and data are under `build/astra-batch/`. A01/A02 used a Python assertion against `make -n check` before and after, followed by the component suite. A03–A10 used `python3 -m unittest discover -s tools -p test_ramtest.py` before and after each change. All ten before checks exited 1. All ten final targeted checks exited 0. A10's immediate first after-run exited 1 and read the previous 25-second default during a same-size, same-second edit. The isolated Python bytecode-cache rerun and final ordinary check both passed. Each build/test exit was taken from the process itself.

## Verification and measurements

| Command or procedure | Exit | Evidence |
|---|---:|---|
| Baseline `make check` | 0 | 181 Python tests, rcl2 self-test and five C suites. Includes W6's 18 installer and seven networking cases. |
| Final `make check` | 0 | 228 Python tests, the rcl2 suites and the l2pm component check. |
| `make -C rcl2 BUILD="$PWD/build/astra-batch/rcl2" test` | 0 | Clean independent build, self-test 44 KiB, five C test programs. ELF 15,568 B, SHA-256 `07fb2c69a9b179f274a5fff96a32a88e8b411e4fb910f65064cdd48192ee3a3b`, matching variant staging. |
| Isolated `make -C build/astra-batch/pkg CC="$PWD/build/bin/musl-gcc" check` with private L2PM_ROOT | 0 | Fresh static build 100,472 B, self-test 164 KiB. Second build in pkg2 and byte-for-byte cmp exited 0. SHA-256 `847d649f9627815d6819b35290a6d54956e47dba834640637027c39e5a20dbde`. Both use the component Makefile. |
| `python3 tools/lowram_matrix.py --iso build/astra-batch/v3.iso --sizes 10 --modes bios --smp 2 --settle 40 --outdir build/astra-batch/bios` | 0 | Real ISO, KVM. 5,440 KiB MemTotal, 3,812 KiB used, 4,800 KiB reserved, spawn OK. Serial log reports **36 KiB PID 1 resident**. |
| `python3 tools/ramtest.py --iso build/astra-batch/v3.iso --ram 10` | 0 | Independent BIOS run, 3,820 KiB used. Kernel 7.3.3-l2-lowram. |
| Same command with `--uefi --ram 128` | 0 | Real ISO and OVMF, 3,980 KiB used of 118,500 KiB. |
| `python3 tools/test-live-install.py` | 0 | Hardware ISO installed from within the live guest onto a disposable disk. Installed shell booted. |
| Private 256 MiB virtio target creation, in-guest installation + poweroff, then `python3 tools/ramtest.py --disk build/astra-batch/installed.img --ram 256` | 0 | Installed 7.3.3-l2, root mounted ext4 rw, 5,192 KiB used of 250,512 KiB. |
| Direct kernel sweep with the v3 bzImage, corresponding initramfs, config/cmdline and `--sizes 10M --settle 40` | 0 | 3,816 KiB used, 4,800 KiB reserved. This run bypassed Limine. |
| `git diff --check`, `git diff --check 7388d77c7..HEAD`, `git status --short` | 0 | Batch patches pass whitespace checks. Pre-existing dirty files are listed in the restoration section. |

All guest memory results used QEMU SMP2 and a 40-second settle. The v3 build has SMP disabled internally. The pinned ISO was 5,961,728 B with SHA-256 `01c784a1ce6fca03d36d4090b6f4b20098680972833350c7c70958f351fd0c3d`. The hardware ISO was 8,650,752 B, SHA-256 `3812611e1f9276268fd10ea005dd54d3688ef4e72a9a965598fe11dccbbedf7f`. Both pass the strict 10 MiB gate. The v3 kernel and initrd hashes were `4d38a2a76248065af70d8f9bd73d5f65f624604a48c24eb0eaea291c20048530` and `a6974f5aba2209eb3ac09ef0676ce7e76e7eff653398adcb44ae9680f28c91ce`. The built kernel config enables serial, ISO9660 and virtio block and disables ext4, INET, SMP and VT. The 10 MiB result covers the serial VM. The ext4 install test used the hardware kernel.

## Reconstructed original backlog

IDs 1–100 below retain the original titles. Their detailed acceptance evidence and ownership live in the numbered [campaign ledger](armageddon-progress.md), the [second ledger](armageddon-ii-progress.md) and the [release assessment](armageddon/release-readiness.md). Status is scoped to the named mission. A successful test of a subsystem establishes only the behavior exercised. Existing evidence is retained for completed historical audits. PENDING denotes no mission-specific implementation identified in these records. The campaign also contains deliberately explored boot failures, including v3's 8 MiB BIOS failure. A failed exploratory size does not invalidate a passing 10 MiB acceptance test.

| ID | Mission | Status | Evidence or remaining condition |
|---:|---|---|---|
| 01 | Analyze complete x86_64 boot process | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 02 | Audit entire current kernel configuration | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 03 | Profile kernel memory allocation during early boot | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 04 | Investigate physical memory reservation | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 05 | Optimize kernel image size | BLOCKED | Default-kernel builder assignment remains open. |
| 06 | Optimize kernel decompression | PARTIAL | Numbered ledger records work and remaining scope. |
| 07 | Analyze page-table memory overhead | PARTIAL | Numbered ledger records work and remaining scope. |
| 08 | Minimize slab allocator overhead | PARTIAL | Numbered ledger records work and remaining scope. |
| 09 | Investigate init memory reclamation | PARTIAL | Numbered ledger records work and remaining scope. |
| 10 | Validate low-memory kernel boot reliability | COMPLETED | Pinned v3 BIOS matrix and independent shell boot at 10 MiB. |
| 11 | Audit BIOS bootloader implementation | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 12 | Audit UEFI bootloader implementation | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 13 | Analyze initramfs memory placement | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 14 | Implement reproducible initramfs generation | COMPLETED | Engineering worktree 14-initramfs-repro.txt records matching hashes on two assemblies. |
| 15 | Validate compression format compatibility | PARTIAL | Numbered ledger records work and remaining scope. |
| 16 | Develop boot-stage instrumentation | PENDING | No dedicated implementation identified. |
| 17 | Investigate KASLR-related boot failures | PARTIAL | Numbered ledger records work and remaining scope. |
| 18 | Validate memory-map edge cases | PENDING | No dedicated implementation identified. |
| 19 | Test boot behavior under memory pressure | PARTIAL | Numbered ledger records work and remaining scope. |
| 20 | Automate QEMU boot regression testing | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 21 | Audit PID1 process lifecycle | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 22 | Optimize baseline PID1 memory usage | COMPLETED | W1/W7 implementation and independent lifecycle suites. Boot RSS meets target. |
| 23 | Implement service dependency resolution | PENDING | No dedicated implementation identified. |
| 24 | Implement parallel service startup | PARTIAL | Once and respawn launch asynchronously. No separate dependency-aware startup gate. |
| 25 | Implement crash recovery | COMPLETED | W1/W7 implementation and independent lifecycle suites. Boot RSS meets target. |
| 26 | Implement watchdog supervision | PENDING | No dedicated implementation identified. |
| 27 | Implement service health monitoring | PENDING | No dedicated implementation identified. |
| 28 | Implement resource controls | PARTIAL | Live scratch caps verified. Broader per-service controls remain open. |
| 29 | Implement emergency recovery behavior | COMPLETED | W1/W7 implementation and independent lifecycle suites. Boot RSS meets target. |
| 30 | Run lifecycle and fault-injection tests | COMPLETED | W1/W7 implementation and independent lifecycle suites. Boot RSS meets target. |
| 31 | Audit the minimal shell | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 32 | Investigate libc memory overhead | PARTIAL | Numbered ledger records work and remaining scope. |
| 33 | Evaluate musl, glibc, and static linking | PARTIAL | Numbered ledger records work and remaining scope. |
| 34 | Optimize essential userland utilities | PENDING | No dedicated implementation identified. |
| 35 | Implement minimal process inspection | PARTIAL | BusyBox process tools and l2net present. Full numbered acceptance criteria unavailable. |
| 36 | Implement minimal networking utilities | PARTIAL | BusyBox process tools and l2net present. Full numbered acceptance criteria unavailable. |
| 37 | Investigate device management | PARTIAL | Existing userspace and guest tests cover subsets. Full campaign verification open. |
| 38 | Test filesystem mounting and recovery | PARTIAL | Existing userspace and guest tests cover subsets. Full campaign verification open. |
| 39 | Minimize binary and runtime dependencies | PARTIAL | Existing userspace and guest tests cover subsets. Full campaign verification open. |
| 40 | Benchmark minimal interactive userspace | PARTIAL | Existing userspace and guest tests cover subsets. Full campaign verification open. |
| 41 | Design L2PM package metadata | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 42 | Implement package installation | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 43 | Implement dependency resolution | PARTIAL | Numbered ledger records work and remaining scope. |
| 44 | Implement transactional upgrades | PARTIAL | W2 rollback and repair suites pass. Host power-loss and multiwriter guarantees outstanding. |
| 45 | Implement rollback | COMPLETED | Kept generations, rollback intent recovery and GC covered by transaction tests. |
| 46 | Implement package signatures | BLOCKED | Signing trust model and repository format need a decision. |
| 47 | Implement repository metadata | BLOCKED | Signing trust model and repository format need a decision. |
| 48 | Implement package caching | PARTIAL | Numbered ledger records work and remaining scope. |
| 49 | Implement source package builds | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 50 | Test package database corruption recovery | PARTIAL | W2 rollback and repair suites pass. Host power-loss and multiwriter guarantees outstanding. |
| 51 | Audit all existing build scripts | PARTIAL | Numbered ledger records work and remaining scope. |
| 52 | Implement reproducible builds | PARTIAL | Numbered ledger records work and remaining scope. |
| 53 | Implement source integrity verification | PARTIAL | Numbered ledger records work and remaining scope. |
| 54 | Implement cross-compilation support | PENDING | No dedicated implementation identified. |
| 55 | Implement dependency-aware build scheduling | PENDING | No dedicated implementation identified. |
| 56 | Implement incremental build caching | PARTIAL | Build cache, artifact verification or build/check target exists. Integrated release gate still open. |
| 57 | Implement automated compiler diagnostics | PENDING | No dedicated implementation identified. |
| 58 | Implement artifact integrity checks | PARTIAL | Build cache, artifact verification or build/check target exists. Integrated release gate still open. |
| 59 | Implement one-command ISO creation | PARTIAL | Build cache, artifact verification or build/check target exists. Integrated release gate still open. |
| 60 | Build an automated release verification pipeline | PARTIAL | Build cache, artifact verification or build/check target exists. Integrated release gate still open. |
| 61 | Audit kernel attack-surface assumptions | PARTIAL | Numbered ledger records work and remaining scope. |
| 62 | Audit PID1 privilege handling | PARTIAL | Numbered ledger records work and remaining scope. |
| 63 | Audit package signature validation | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 64 | Audit filesystem permissions | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 65 | Investigate unsafe memory operations | PARTIAL | Parser and W6 or W2 regression work exists. Broader audit, fault coverage or P2 sign-off remains. |
| 66 | Audit subprocess execution | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 67 | Implement malformed-input testing | PARTIAL | Parser and W6 or W2 regression work exists. Broader audit, fault coverage or P2 sign-off remains. |
| 68 | Investigate race conditions | PARTIAL | Parser and W6 or W2 regression work exists. Broader audit, fault coverage or P2 sign-off remains. |
| 69 | Evaluate reproducible-build supply-chain security | PARTIAL | Numbered ledger records work and remaining scope. |
| 70 | Execute regression tests for confirmed vulnerabilities | PARTIAL | Parser and W6 or W2 regression work exists. Broader audit, fault coverage or P2 sign-off remains. |
| 71 | Measure cold boot latency | PARTIAL | Historical timing, attribution and trim data. Complete comparative series missing. |
| 72 | Measure steady-state memory usage | COMPLETED | Pinned v3 BIOS matrix and independent shell boot at 10 MiB. |
| 73 | Measure process creation overhead | PENDING | No dedicated implementation identified. |
| 74 | Measure filesystem overhead | PENDING | No dedicated implementation identified. |
| 75 | Measure shell startup overhead | PENDING | No dedicated implementation identified. |
| 76 | Measure service startup latency | PENDING | No dedicated implementation identified. |
| 77 | Measure package installation performance | PENDING | No dedicated implementation identified. |
| 78 | Measure build-system performance | PENDING | No dedicated implementation identified. |
| 79 | Identify top five performance bottlenecks | PARTIAL | Historical timing, attribution and trim data. Complete comparative series missing. |
| 80 | Implement and validate targeted optimizations | PARTIAL | Historical timing, attribution and trim data. Complete comparative series missing. |
| 81 | Test guest RAM sizes 10 MiB–256 MiB | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 82 | Test multiple QEMU CPU models | PENDING | No dedicated implementation identified. |
| 83 | Test BIOS configurations | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 84 | Test available UEFI configurations | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 85 | Test different initramfs compression modes | PARTIAL | Gzip, guest ext4 or boot flags covered by existing configurations. Wider matrix incomplete. |
| 86 | Test filesystem compatibility | PARTIAL | Gzip, guest ext4 or boot flags covered by existing configurations. Wider matrix incomplete. |
| 87 | Test kernel command-line variations | PARTIAL | Gzip, guest ext4 or boot flags covered by existing configurations. Wider matrix incomplete. |
| 88 | Test clean shutdown and reboot | COMPLETED | Numbered ledger links audit, implementation or matrix evidence. |
| 89 | Test fault recovery | PARTIAL | Numbered ledger records work and remaining scope. |
| 90 | Create documented compatibility matrix | PARTIAL | See release assessment and measured gates above. Full integrated hardware release remains open. |
| 91 | Review every architectural assumption | PARTIAL | See release assessment and measured gates above. Full integrated hardware release remains open. |
| 92 | Reproduce all claimed improvements | PARTIAL | See release assessment and measured gates above. Full integrated hardware release remains open. |
| 93 | Search for hidden regressions | PARTIAL | See release assessment and measured gates above. Full integrated hardware release remains open. |
| 94 | Review all benchmark methodologies | PARTIAL | See release assessment and measured gates above. Full integrated hardware release remains open. |
| 95 | Audit final build artifacts | PARTIAL | See release assessment and measured gates above. Full integrated hardware release remains open. |
| 96 | Validate final memory budget | PARTIAL | See release assessment and measured gates above. Full integrated hardware release remains open. |
| 97 | Validate final boot reliability | PARTIAL | See release assessment and measured gates above. Full integrated hardware release remains open. |
| 98 | Produce complete technical documentation | PARTIAL | See release assessment and measured gates above. Full integrated hardware release remains open. |
| 99 | Create prioritized remaining-defects backlog | PARTIAL | See release assessment and measured gates above. Full integrated hardware release remains open. |
| 100 | Deliver verified release candidate if all gates pass | BLOCKED | Integrated hardware build ownership and release security posture need explicit decisions. |

### Missions 101–300

The handoffs assign domains and W1–W7 to ranges. They provide no individual titles or acceptance criteria for these 200 IDs. Each individual ID in the inclusive ranges below is **BLOCKED for identification and classification of its implementation** pending the original numbered manifest. The domain evidence is actionable without inventing individual assignments or granting 20 completions for one verified workstream.

| Individual IDs covered | Recovered workstream | Verified scope and gaps |
|---|---|---|
| 101–120 | Kernel/boot, also W7 | Live scratch caps and v3 boot evidence exist. |
| 121–140 | Kernel/boot | Variant validated. Default trim and final integration ownership open. |
| 141–160 | rcl2, W1 | Lifecycle and parser regression suites pass. Named workstream complete. |
| 161–180 | No domain specified in the recovered range mapping | Original manifest needed even to assign the domain. |
| 181–200 | l2pm, W2 | Transaction and recovery regression suites pass. Independent worktree findings need reconciliation. |
| 201–220 | Build, W2/W5 | Expanded check target, identical component l2pm builds. Integrated build-path alignment remains open. |
| 221–240 | Networking/userspace | W6 shell fixes present. N5/P2 require coordinated integration. |
| 241–260 | QEMU matrix | Real BIOS/UEFI, disk and direct boot evidence available. |
| 261–280 | Security, W4/W6 | Shell marker and parser guards exercised. Pack authenticity depends on P2. |
| 281–300 | Release/docs, W5 | Readiness assessment and local gates exist. Hardware release authorization outstanding. |

## Remaining blockers and next batch

1. Reconcile the unmerged independent verification worktree and its outstanding package fuzz/fault findings with the lane owner. Two identical isolated l2pm component builds establish same-toolchain reproducibility on this host. Production builder alignment, concurrent writers and storage power-loss behavior need separate evidence.
2. Obtain the original 101–300 manifest and confirm ownership for default-kernel mission 05, P2 build-side pinning, N5 integration and the final integrated hardware image. The installed-disk pass used an existing hardware ISO and cannot sign off newly compiled W6 or W7 contents in a hardware release.
3. Resolve trust policy for package signatures and hardware-pack authentication. The existing live-medium marker and local checksums do not authenticate publishers. The root serial console and reduced kernel mitigations constrain where v3 can be deployed.
4. After integration, rerun pinned media through the supported RAM/firmware matrix and the full stability gate. This batch did not run a 72-hour soak, 100-boot endurance campaign, physical power-loss injection or bare-metal validation. The remaining work is outside these ten missions.
