# L2 ARCHNEMESIS — Neon Campaign

**Status:** ACTIVE (launch 2026-11-26 16:30 ICT)
**Coordinator:** Neon — Gemini 4 Neon (Medium)
**Workers:** Argon, Carbon, Hydrogen
**Base:** `campaigns/archnemesis-sol` @ `02717ddaf` (verified checkpoint)
**Campaign branch:** `campaigns/archnemesis-neon`
**Workspace:** `/home/maiake/OpenBot/Agents/agent-ed44d231-41cf-416e-b3d7-8ce3907ab027/archnemesis/neon`
**Host:** Ryzen 7 7735HS, 16 CPUs, ~12.7 GiB RAM, KVM available

## Safety invariants
- The operator checkout `/home/maiake/L2` is read-only for this campaign; its uncommitted changes are preserved untouched.
- No push, no merge to `main`, no destructive Git, no host configuration changes, no paid usage.
- Max aggregate compilation parallelism 8; target load <= 6; no simultaneous kernel builds.

## Baseline (verified by Neon on `05bd3b19f`)
- Prior checkpoint: 10 commits on `campaigns/archnemesis-sol`; full host suite passed per `campaigns/archnemesis/gpt/HANDOFF.md`.
- `make check` (`CC=<shared>/bin/musl-gcc`): **PASS (`rc=0`)**, wall time **21s**, **241/241** Python unit tests + `rcl2` C tests (`--selftest` resident **44 KiB**) + `l2pm check` (`rss: 160 KiB`). Log: `campaigns/archnemesis/evidence/baseline-make-check.log`.
- `./l2 doctor`: **PASS (`rc=0`)**, 30/30 host tools ok, core ISO **8.2 MiB / 10 MiB**, `build/out/l2pm` selftest RSS **156 KiB**. Log: `campaigns/archnemesis/evidence/baseline-doctor.log`.
- Artifact sizes & SHA-256 (`campaigns/archnemesis/evidence/baseline-artifacts.sha256`):
  - `build/out/l2.iso`: **8,636,416 B** (8.24 MiB) `66ddd6aa…`
  - `build/out/vmlinuz-l2`: **2,831,360 B** (2.70 MiB) `4848e46f…`
  - `build/out/initramfs-l2`: **422,838 B** (412.9 KiB) `e4ac7d49…`
  - `build/out/rcl2`: **17,184 B** (`e6dd58ef…`), `build/out/l2pm`: **88,184 B** (`efc32819…`), `build/out/l2wifi`: **63,416 B** (`d3f7957f…`), `build/out/l2gpt`: **42,672 B** (`e977e0e5…`), `build/out/l2audio`: **63,224 B** (`801d092e…`).
- QEMU boot baseline (`tools/ramtest.py --iso build/out/l2.iso --ram 256M --settle 40 --budget 10240 --pid1-budget 50`, KVM, `-smp 2`):
  - **BIOS 256 MiB**: **PASS** — idle RAM **5,524 KiB** used / 250,516 KiB MemTotal, PID 1 RSS **40 KiB**, `7.3.3-l2`, clean poweroff (`evidence/baseline-boot-bios-256m.log`).
  - **UEFI 256 MiB**: **PASS** — idle RAM **5,700 KiB** used / 244,592 KiB MemTotal, PID 1 RSS **48 KiB**, `7.3.3-l2`, clean poweroff (`evidence/baseline-boot-uefi-256m.log`).
  - **Core BIOS 10 MiB** (supervisor baseline finding): **FAIL** — `build/out/l2.iso` does not reach `L2#` prompt at 10 MiB guest RAM. The low-RAM profile (`l2-lowram`) is the 10 MiB boot path; Argon assigned to investigate core-vs-lowram at 8/9/10 MiB.

## Mission board (M01–M20)

| ID | Owner | Status | Commit | Evidence |
|---|---|---|---|---|
| M01 | Carbon | IN_PROGRESS | — | — |
| M02 | Carbon | IN_PROGRESS | — | — |
| M03 | Carbon | IN_PROGRESS | — | — |
| M04 | Carbon | IN_PROGRESS | — | — |
| M05 | Carbon | IN_PROGRESS | — | — |
| M06 | Carbon | PENDING | — | — |
| M07 | Carbon | PENDING | — | — |
| M08 | Carbon | PENDING | — | — |
| M09 | Carbon | PENDING | — | — |
| M10 | Carbon | PENDING | — | — |
| M11 | Argon | IN_PROGRESS | — | SOL-002 (`c3f5ae7b0`) on base; verifying & completing |
| M12 | Argon | IN_PROGRESS | — | — |
| M13 | Argon | IN_PROGRESS | — | SOL-001 (`98a88a395`) + SOL-005 (`f6cd73de4`) on base; verifying & completing |
| M14 | Argon | IN_PROGRESS | — | — |
| M15 | Argon | IN_PROGRESS | — | SOL-003 (`f00114569`) on base; verifying & completing |
| M16 | Hydrogen | IN_PROGRESS | — | SOL-009 (`02717ddaf`) on base; verifying & completing |
| M17 | Hydrogen | IN_PROGRESS | — | — |
| M18 | Hydrogen | IN_PROGRESS | — | — |
| M19 | Hydrogen | IN_PROGRESS | — | SOL-004 (`f19026121`) on base; verifying & completing |
| M20 | Hydrogen | IN_PROGRESS | — | — |

## Campaign log
- `16:22` — Isolated worktree created from the verified SOL checkpoint; ledger imported from the primary checkout (read-only).
- `16:30` — Swarm launch (Neon coordinator; Argon, Carbon, Hydrogen workers in isolated worktrees).
- `16:35` — Baseline verified: `make check` 241/241 PASS (21s), `./l2 doctor` PASS (ISO 8.2 MiB), BIOS 256M 5,524 KiB (PID 1 40 KiB), UEFI 256M 5,700 KiB (PID 1 48 KiB).
