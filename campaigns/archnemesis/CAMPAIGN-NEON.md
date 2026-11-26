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

## Baseline (to be completed by Neon)
- Prior checkpoint: 10 commits on `campaigns/archnemesis-sol`; full host suite passed per `campaigns/archnemesis/gpt/HANDOFF.md`.
- New worktree `make check`: _pending_
- Artifact sizes: _pending_
- QEMU boot matrix: _pending_

## Mission board (M01–M20)

| ID | Owner | Status | Commit | Evidence |
|---|---|---|---|---|
| M01 | Carbon | PENDING | — | — |
| M02 | Carbon | PENDING | — | — |
| M03 | Carbon | PENDING | — | — |
| M04 | Carbon | PENDING | — | — |
| M05 | Carbon | PENDING | — | — |
| M06 | Carbon | PENDING | — | — |
| M07 | Carbon | PENDING | — | — |
| M08 | Carbon | PENDING | — | — |
| M09 | Carbon | PENDING | — | — |
| M10 | Carbon | PENDING | — | — |
| M11 | Argon | PENDING | — | — |
| M12 | Argon | PENDING | — | — |
| M13 | Argon | PENDING | — | — |
| M14 | Argon | PENDING | — | — |
| M15 | Argon | PENDING | — | — |
| M16 | Hydrogen | PENDING | — | — |
| M17 | Hydrogen | PENDING | — | — |
| M18 | Hydrogen | PENDING | — | — |
| M19 | Hydrogen | PENDING | — | — |
| M20 | Hydrogen | PENDING | — | — |

## Campaign log
- `16:22` — Isolated worktree created from the verified SOL checkpoint; ledger imported from the primary checkout (read-only).
- `16:30` — Swarm launch (Neon coordinator; Argon, Carbon, Hydrogen workers).
