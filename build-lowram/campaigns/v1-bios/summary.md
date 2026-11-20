# L2 low-RAM boot matrix

- generated (UTC): 2026-11-20T14:00:35+00:00
- iso: build-lowram/out/l2-lowram.iso
- smp: 2
- settle_s: 40.0
- timeout_s: 180.0
- accel: kvm
- forensics: False

| MiB | mode | status | signature/reason | last kts | MemTotal | used | reserved | spawn | log |
|----:|------|--------|------------------|---------:|---------:|-----:|---------:|-------|-----|
| 12 | bios | PASS |  | — | 6064 | 5032 | 6224 | ok | 12M-bios.log |
| 16 | bios | PASS |  | — | 10096 | 5064 | 6288 | ok | 16M-bios.log |
| 32 | bios | PASS |  | — | 26224 | 5092 | 6544 | ok | 32M-bios.log |
| 256 | bios | PASS |  | — | 252024 | 5088 | 10120 | ok | 256M-bios.log |

PASS 4 / FAIL 0 / SKIPPED 0 — exit 0 only if no FAIL.

used = MemTotal - MemAvailable (lowest of 3 samples after settle); reserved = installed - MemTotal.
