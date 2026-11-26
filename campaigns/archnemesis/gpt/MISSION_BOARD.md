# Second Front mission board

Batch 1 uses GPT-6.2 Sol at High reasoning on all four agents. The coordinator verified the model and effort in each active session record. The campaign started from `380a3651e0ecac9a8fe785d654361c37698311e4`.

| Mission | Owner and files | Evidence before and after | Integration commit |
| --- | --- | --- | --- |
| SOL-001 | Titanium. `tools/l2-lowram` and its build tests | Four of five forced configuration failures previously published a fixture image. All five now fail before publication. | `98a88a395` |
| SOL-002 | Titanium. `tools/patch-kernel-ram.py` and the same build tests | A late NVMe hunk mismatch changed an earlier source file. Two subsequent regression attempts report the intended mismatch and preserve that file. The actual kernel source passes patch validation. | `c3f5ae7b0` |
| SOL-003 | Iridium. `l2install` and its tests | The disk installer continued after SIGINT and SIGTERM. It now exits 130 and 143 before partitioning. The image test observes exactly one simulated loop detach. | `f00114569` |
| SOL-004 | Osmium. Stability harness and its tests | A failed directory change or scratch write used to produce the crash-readiness marker. Both failures now suppress it. A successful write still produces it. | `f19026121` |
| SOL-005 | Tungsten. `.gitignore` and `tools/verify-lowram.py` | A clean checkout failed the release-screen test because the script was missing. It now rejects wrong digests and images at the size limit. Independent verification also checked the pinned-snapshot flow. | `f6cd73de4` |

Tungsten ran the worker regressions after integration. Osmium independently ran the release-screen regression against the parent and fixed revisions. See [BENCHMARKS.md](BENCHMARKS.md) for integration results and [FINDINGS.md](FINDINGS.md) for remaining limits.

## Proposed second batch after ownership is resolved

| Mission | Verification goal |
| --- | --- |
| SOL-006 | Exercise the primary M11 patcher against late mismatch and retry fixtures and resolve overlap with SOL-002. |
| SOL-007 | Check the M13 clean checkout release screen and confirm failed configuration cannot publish an image. |
| SOL-008 | Independently qualify M15 installer interruption, including loop and mount acquisition in disposable fixtures. |
| SOL-009 | Verify M19 requires a responsive shell, the installed ext4 root, a successful scratch write and clean poweroff where applicable. |
| SOL-010 | Repeat an integrated build in two private directories and compare image hashes, BIOS boot floor and settled RAM. |
