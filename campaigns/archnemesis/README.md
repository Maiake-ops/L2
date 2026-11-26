# L2: ARCHNEMESIS — Engineering Campaign 01

## Overview
ARCHNEMESIS is an autonomous, measurement-driven hardening, optimization, and reproducibility engineering campaign for the **L2** minimal Linux distribution (`campaigns/armageddon-ii` base commit `380a3651e`).

## Campaign Targets (Unverified Until Measured)
- Booted idle RAM consumption `< 10 MiB` (`< 10,240 KiB`) on standard hardware and low-RAM profiles.
- Boot-resident `rcl2` PID 1 memory footprint `<= 50 KiB` (`VmRSS`).
- Core BIOS+UEFI ISO (`build/out/l2.iso`) strictly `< 10 MiB`.
- Deterministic, byte-reproducible builds with zero host-path leaks.
- Rock-solid PID 1 lifecycle, transactional `l2pm` durability, co-signed medium pack integrity, and real-hardware / low-RAM reliability.

## Campaign Rules
1. **No fabricated measurements** — every number must come verbatim from executed commands.
2. **No weakened tests** — never alter assertions to hide bugs; document observed baseline failures honestly.
3. **Disposable testing** — run destructive, fault-injection, and scratch-build tests only under `/tmp`.
4. **Atomic commits & exclusive file ownership** — one clean commit per mission; never touch pre-existing unrelated dirty files (`desktop/external.mk`, `desktop/rootfs/bin/netsurf`, `lib/desktop.sh`, `tools/test-desktop.py`, `media/`).
5. **No destructive git ops or unauthorized pushes**.

## Unit Roster
| Unit | Role | Primary Domain |
|---|---|---|
| **M** (`@coordinator`) | Campaign Lead & Integration | Scheduling, review, cherry-pick integration |
| **HELIUM** | Kernel & Low-RAM Optimization | `config/kernel.*`, `config/cmdline`, `tools/patch-kernel-ram.py`, `tools/l2-lowram`, `tools/verify-lowram.py` |
| **NEON** | `rcl2` & PID 1 Reliability | `rcl2/*`, `config/rcl2.conf`, `overlay/bin/shutdown`, `overlay/etc/l2/serial-getty`, `tools/test_rcl2_lifecycle.py` |
| **CARBON** | `l2pm` Package Manager | `l2pm/*`, `tools/test_l2pm_*.py` |
| **HYDROGEN** | Build Pipeline, Installer & GPT | `./l2`, `l2install`, `l2gpt/*`, `l2fetch`, `tools/mkimage.sh` |
| **ARGON** | Security, Network Packs & Clients | `overlay/bin/{l2net,openssl}`, `overlay/etc/l2/udhcpc.script`, `l2wifi/*`, `l2audio/*`, `config/openssl.cnf` |
| **XENON** | Testing, Benchmarking, Reproducibility & Independent Verification | `Makefile`, `tools/{ramtest,lowram_matrix,test-stability,test-live-install,bench-repro}.py`, `tools/test_l2pm_adversarial.py`, `campaigns/archnemesis/*` |

## Ledger Files
- [`baseline.md`](baseline.md): Phase 00 verified pre-campaign measurements and artifact inventory.
- [`batch-01.md`](batch-01.md): Batch 1 (M01–M20) mission matrix, ownership, acceptance criteria, and verification log.
