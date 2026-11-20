# L2 low-RAM boot matrix

- generated (UTC): 2026-11-20T13:55:11+00:00
- iso: build-lowram/out/l2-lowram.iso
- smp: 2
- settle_s: 40.0
- timeout_s: 180.0
- accel: kvm
- forensics: False

| MiB | mode | status | signature/reason | last kts | MemTotal | used | reserved | spawn | log |
|----:|------|--------|------------------|---------:|---------:|-----:|---------:|-------|-----|
| 10 | bios | FAIL | oom-deadlock | 0.380176 | — | — | — | — | 10M-bios.log |

PASS 0 / FAIL 1 / SKIPPED 0 — exit 0 only if no FAIL.

used = MemTotal - MemAvailable (lowest of 3 samples after settle); reserved = installed - MemTotal.
