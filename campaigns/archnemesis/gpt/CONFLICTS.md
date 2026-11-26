# Cross-swarm ownership

The starting commit was `380a3651e0ecac9a8fe785d654361c37698311e4` on `campaigns/armageddon-ii`. The coordinator and each worker used a separate worktree and local branch based on that commit. The original checkout had local desktop edits and untracked media.

| Repository evidence at initialization | Scope kept with the other workstream |
| --- | --- |
| Gemini Neon local changes | `rcl2/src/main.c`, `rcl2/src/syscalls.h`, `overlay/bin/shutdown` |
| Gemini Carbon local changes | `l2pm/l2pm.c`, `tools/test_l2pm_security.py` |
| Gemini Hydrogen local changes | `l2gpt/l2gpt.c`, `tools/test_l2gpt.py` |
| Main checkout and the Gemini worktrees | Desktop build logic, NetSurf launcher, desktop verification, untracked media |
| Qwen engineering worktree | Package build and validation changes, Wi-Fi work and its regression tests |

The initial ownership check used Git refs, worktree status and the committed ARMAGEDDON ledgers. No ARCHNEMESIS ledger or changes to the five selected source scopes appeared in that check. By final verification the primary checkout contained a new ARCHNEMESIS ledger and edits to the verification harnesses. Gemini's Helium worktree also had a patcher refactor that replaced the same paths through `patch_file` and `main` that SOL-002 had changed.

| Second Front | Primary ledger assignment | Coordination needed |
| --- | --- | --- |
| SOL-001 and SOL-005 | M13, Helium | Compare configuration failure handling and the tracked release verifier. |
| SOL-002 | M11, Helium | The patcher's validation and publication changes overlap directly. Validate the partial-write regression against the primary version before choosing a patch. |
| SOL-003 | M15, Hydrogen | The primary plan includes separate interrupt exit traps and cleanup changes. Compare both interruption fixtures. |
| SOL-004 | M19, Xenon | The primary checkout has concurrent changes to the stability harness and its tests. Check the scratch write regression against those edits. |

The primary ledger also assigns the next boot and installation harness work to Xenon. The records are `campaigns/archnemesis/README.md`, `baseline.md` and `batch-01.md` in the primary checkout. No direct Gemini communication channel was available. Batch 2 is paused under the campaign's stop rule until the overlapping ownership and integration decisions are resolved.

The worker commits were reviewed and cherry-picked onto `campaigns/archnemesis-sol`. The campaign made no remote changes. Build outputs and logs stayed in the isolated campaign directories.
