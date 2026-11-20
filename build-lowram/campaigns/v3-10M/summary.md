# L2 low-RAM boot matrix

- generated (UTC): 2026-11-20T14:38:24+00:00
- iso: build-lowram/out/l2-lowram.iso
- smp: 2
- settle_s: 40.0
- timeout_s: 180.0
- accel: kvm
- forensics: False

| MiB | mode | status | signature/reason | last kts | MemTotal | used | reserved | spawn | log |
|----:|------|--------|------------------|---------:|---------:|-----:|---------:|-------|-----|
| 10 | bios | PASS |  | — | 5440 | 3836 | 4800 | ok | 10M-bios.log |

PASS 1 / FAIL 0 / SKIPPED 0 — exit 0 only if no FAIL.

used = MemTotal - MemAvailable (lowest of 3 samples after settle); reserved = installed - MemTotal.
