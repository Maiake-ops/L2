# Mission 51 — build-script audit (2026-11-20, PARTIAL: l2 orchestrator done)

Scope covered: `l2` (1880 lines — build/repro/download/update/ISO paths),
root `Makefile`, `l2pm/Makefile` + `rcl2/Makefile` (via earlier reads),
`tools/mkimage.sh` (README-described, not line-audited). PENDING: full
`l2install` destructive-path review (find_medium verified only), overlay
scripts beyond l2net (F1 in security report), l2wifi/Makefile, mkimage.sh.
No source changes made; fixes queued below (our build-infra lane, deferred
while any peer ./l2 build runs — bash reads scripts incrementally).

## Structure (verified by reads)
- `l2` bash orchestrator: deps per-distro, sources.lock pins, component
  builders (musl→headers→kernel→dash→busybox→rcl2→l2pm→l2wifi→libnl→wpa→
  openssl→firmware→packs→limine→rootfs→initramfs→iso), gum TUI + plain
  fallback, doctor/ram/bootiso/test menus. `Makefile` is convenience only.
- Repro hygiene (good): init_epoch exports SOURCE_DATE_EPOCH (lock `epoch=`,
  default 1767225600) + TZ=UTC LC_ALL=C LANG=C umask 022; kernel
  KBUILD_BUILD_TIMESTAMP/USER/HOST/VERSION fixed; -ffile-prefix-map /
  -fdebug-prefix-map; initramfs sorted newc + --owner=0:0 + gzip -9 -n;
  ISO epoch touches + xorriso SOURCE_DATE_EPOCH + limine srand(epoch) patch
  + MBR ID pinned to L2\0\1 at 440; l2pm deterministic ustar + --check.
  VERIFIED this session: kernel rebuild byte-identical (0fa6cfc8…).
- Build isolation: each builder re-extracts source (rm -rf dir; tar) —
  manual build/src edits don't survive (documented for mission 05).
  JOBS=${JOBS:-8}, LOAD=${L2_LOAD:-6} (-l load cap) — contention-aware.
- Kernel config: tinyconfig + merge_config fragment(s) (+kernel.local),
  =n normalized to "# … is not set", olddefconfig, dropped-symbol report
  (check_kernel_dropped → build/logs/kernel-dropped.txt + doctor re-checks
  NVMe/AHCI/USB/HID/PS2/EFI-fb/ACPI/wifi). Good silent-loss protection.
- ISO gate: _make_iso enforces L2_ISO_LIMIT_MB=10 (die unless
  L2_ALLOW_BIG_ISO=1); wifi ISO 96 MiB gate. UEFI El Torito must stay
  stock 2.8 MiB Limine image (OVMF constraint, documented).

## Findings (ranked; fix lane = ours unless noted)
1. **dash plaintext-HTTP supply chain** (l2:293 latest_dash resolver,
   l2:327 update download, l2:683 build). Resolver fetches the version
   LIST over http (attacker can advertise a fake "latest"); update pins an
   EMPTY sha over http → MITM hash persisted into sources.lock; build then
   "verifies" against the poisoned pin. Fix: https mirror (gondor supports
   https? verify; else kernel.org-style mirror or git tag pin), refuse
   empty-sha pinning over http, keep lock sha mandatory. (Security report
   §69; missions 53/69.) NUANCE (maiake-f6 independent confirmation,
   worktree build/campaigns/07-reproducible.md cross-ref): shipped
   dash_sha256 is NON-EMPTY ⇒ current builds verify against the existing
   pin; exposure is pin (re)establishment via l2 update/resolver over
   HTTP, not the current build path. They also confirmed kernel
   compile-layer determinism between sessions (rebuild 0fa6cfc8 == their
   snapshot vmlinuz) — mission 52 evidence.
2. **Empty-pin TOFU acceptance** (verify_sha l2:247–258 `[ -n "$want" ] ||
   return 0`; download l2:259–280 warns but accepts; cached-file branch
   accepts ANY existing file when want empty). update_sources pins
   musl/dash/limine this way (kernel uses published sha256sums.asc over
   https — good, but GPG signature of the .asc is NOT checked, only the
   hash extracted). Fix: mark TOFU pins explicitly in lock (e.g.
   `pin_method=tofu`), warn loudly, prefer upstream signed hashes;
   never accept empty-want cached files without re-hash when a pin exists.
3. **curl -L redirect downgrade** (download l2:268): https→http redirect
   followed by default. Fix: `--proto-redir =https` (and `-f` already set).
   wget fallback has no timeout; curl download has connect-timeout but no
   --max-time → stalled-mirror hang. Fix: add --max-time / wget -T.
4. **Non-atomic lock update** (update_sources l2:304–348): sequential
   lock_set calls (each atomic mktemp+mv) — crash mid-update leaves a
   mixed-version lock. Fix: build the whole new lock in one tmp + single mv.
5. **latest_limine resolver vs source mismatch**: resolver reads GitHub
   tags (git ls-remote https), build downloads Codeberg releases (lock
   limine_source). Tags usually mirror; note for consistency (pin resolver
   to the same host as the source).
6. **l2pm tarball cache contrast** (gapmap §48): l2pm hard-fails on cached
   hash mismatch (never re-fetches) while l2 download re-fetches —
   inconsistent policies; L2-ea W2 lane for l2pm side.
7. Minor: `clean()` rm -rf build (destructive but scoped + user-invoked,
   fine); `build_all` continues past wpa/firmware/openssl failures by
   design (core ISO still builds — documented); strip -s on dash/l2pm
   (deterministic); no fsync anywhere in build paths (power-loss →
   corrupt artifacts caught by hash gates on next build; acceptable).

## Installer (partial — full review pending)
- l2install find_medium REQUIRES l2/L2-LIVE marker (123–131) — correct
  trust anchor; contrast l2net F1 (security report). Disk path: typed
  confirmation / --yes, hybrid GPT (1 MiB BIOS stage2 + 256 MiB ESP + ext4),
  limine BIOS+UEFI install, guid(PARTUUID) config, post-install verify
  (README; line-level review pending). Live-install pack (l2gpt/mke2fs/
  mkfs.fat/limine) rides on medium — zero idle RAM (memory-confirmed).
- tools/mkimage.sh: rootless installed-image builder (sgdisk + mkfs.ext4 -d
  + mtools), stops ext4 short of disk end for backup GPT (README) — review
  pending under mission 58/86.

## Regression assessment / limitations
- Read-only audit; no changes. Fixes 1–5 are small, reviewable l2 edits —
  DEFERRED until no peer ./l2 build is running (bash incremental-read
  hazard); coordinate via ledger ping. l2install/overlay/mkimage deep
  review pending (mission 51 stays [~]). Repro claim verified for kernel
  only; full double-build ISO spot-check is mission 52 (heavy-coder does
  one under 05).