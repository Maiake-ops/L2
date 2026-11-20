# Missions 31/32/33 — shell + libc audit (2026-11-20)

Evidence: `l2:build_dash` (676–706), `l2:build_musl`/`make_musl_wrapper`
(349–418, read earlier), `sources.lock` pins, `build/out/` artifact sizes,
README "Why busybox and dash?" + baseline sweep (idle ~5.5 MiB flat).

## Mission 31 — minimal shell (dash 0.5.12) — DONE

- Pinned version + sha256 in sources.lock; verified download; re-extracted
  each build. **Supply-chain flag (missions 53/69): source URL is plain
  HTTP** (`http://gondor.apana.org.au/~herbert/dash/files/`). The sha256 pin
  detects tampering, but the FIRST pin is TOFU over an unencrypted channel.
  Recommend https mirror or signed pin bootstrap; low effort, real gain.
- Build: musl wrapper CC; `CFLAGS=-static -Os -ffunction-sections
  -fdata-sections -ffile-prefix-map=$PWD=. -fdebug-prefix-map=$PWD=.`;
  `LDFLAGS=-static -Wl,--gc-sections`; `strip -s` after install.
  Reproducibility-friendly (prefix maps, no embedded dates, deterministic
  strip). Binary: **145,168 B**.
- Wiring: `/bin/sh → dash`; busybox shells disabled (SH_IS_NONE) so dash is
  the ONLY shell; rcl2 compiled-in default `/bin/dash`; all distro scripts
  (l2fetch, l2install, l2net, udhcpc.script, serial-getty) are POSIX sh —
  consistent, no bashisms in the runtime path (l2 orchestrator itself is
  bash but host-side only).
- Audit verdict: appropriate minimal choice; no custom shell code to
  maintain. Startup-overhead measurement pending (mission 75); interactive
  benchmark pending (mission 40).

## Mission 32 — libc memory overhead — audit done, measurement pending

- musl 1.2.5 pinned; static-only userland; generated `build/bin/musl-gcc`
  wrapper (self-contained, handles Nix gcc `-latomic_asneeded` quirk) — no
  distro musl-gcc required.
- Consequence: no shared libc text across processes; each static binary
  embeds its own libc subset (busybox 374,752 B; dash 145,168 B; l2pm
  88,184 B; l2wifi 63,416 B; rcl2 has NO libc — raw syscalls). At idle only
  ~3–5 processes exist (rcl2, dash getty ×1–2, l2net exited), so the
  duplication cost is bounded; measured idle 5.5 MiB flat confirms the
  trade-off works at this scale.
- Per-process RSS breakdown (VmHWM of dash/busybox/l2pm under load) not yet
  measured — missions 40/72 own that. l2pm selftest prints VmHWM (budget
  enforcement at l2:751–758); README ~130 KiB claim unverified this session.

## Mission 33 — musl vs glibc vs static linking — decision documented

- glibc rejected: NSS/dlopen/locales bloat, poor static-linking story,
  larger per-process footprint — incompatible with <10 MiB budgets.
- Shared musl considered: would dedupe libc text across processes but adds
  ld.so + libc.so mappings and page-cache pressure at idle; with ≤5 idle
  processes the static choice is equal or better, and static binaries are
  immune to library-path attacks and simplify the content-addressed store
  (no runtime lib deps). KEEP STATIC (input for mission 91 architectural
  review). Re-evaluate only if process count grows substantially.

## Regression assessment / limitations
- Read-only audit; no changes. HTTP-URL flag recorded for missions 53/69
  (implementation lane: build infra is ours, but defer edits until no peer
  ./l2 build is running). Measurements (75/40/72) pending.