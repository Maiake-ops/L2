# F1 Phase-2 pack-pin contract — CO-SIGNED (L2-ea 22:20, A1–A3; lead accepts + A2 extension 22:25)

Status: AGREED. Implementation queued: build-side (our l2 lane) after
mission 05 lands (lane dispute RESOLVED 22:50 — recorded split stands,
mission 05 = L2-72 lane); shell-side (L2-ea W6-P2) after our ping.
W6-P1 (marker + fail-closed) proceeds immediately in their lane.
Sequencing: P1 → contract (DONE) → P2 split → mission-70 regression (ours).

Threat: F1 (ring-0 module/firmware
load from untrusted medium, docs/armageddon/61-69-security-posture.md) PLUS
the installed-system fall-through: `/lib/modules/$KREL` short-circuit is the
only guard on installed boots today; if that dir is missing (kernel upgrade
without modules, corruption, partial install), find_pack falls through to
medium search and F1 resurrects — the root-side pin closes this (L2-ea
insight, accepted).

## Artifacts (format contract)
- **Per-pack manifest** (AGREED per-pack, not one global): each pack dir
  carries `pack-manifest` — on the medium at `l2/<pack>/pack-manifest` and
  in build staging. pack ∈ {hw, wifi, install, ssl}. Format: LF lines
  `sha256  relpath` (two spaces, sha256sum style), relpath relative to pack
  root, LC_ALL=C bytewise sort, excludes pack-manifest itself, no absolute
  paths or `..`. Deterministic at build (content hashes; pack files already
  epoch-touched). Rationale: packs are built independently; core ISO may
  ship hw+install without wifi/ssl — per-pack keeps absent packs clean.
- **Pin file**: `/etc/l2/pack-hash`, root:root 0644, in TWO locations:
  (a) initramfs — embedded at ISO build (live/rescue boots);
  (b) ext4 root — written by l2install at install time (installed boots).
  Format: LF lines `sha256  packname` (sha256 OF that pack's pack-manifest,
  plus pack name), sorted. Empty/absent pin ⇒ NO pack trusted (fail-closed).

## Build side — OUR l2 lane (post-mission-05)
- Pack builders (build_hwpack / build_wifipack / build_installpack /
  build_openssl) generate `pack-manifest` into each pack staging dir.
- build_iso computes the pin from the manifests of the packs actually
  included → writes `rootfs/etc/l2/pack-hash` → REBUILDS initramfs →
  _make_iso assembles. Flow change: initramfs is rebuilt inside the ISO
  step after pin injection (packs are assembled after initramfs today).
  Reproducibility preserved: deterministic pin + epoch touches + gzip -n.
- Host-install staging carries manifests + pin (no medium involved) —
  AGREED with L2-ea open question 2.

## Install side — L2-ea lane
- l2install verifies the medium (L2-LIVE + chain below), copies packs, and
  writes root `/etc/l2/pack-hash` from the VERIFIED medium's manifests
  (host path: from build staging). Pin refresh on kernel/pack upgrade goes
  through l2install (documented).
- A3 UPGRADE INVARIANT (ACCEPTED, generalized): ANY update path that
  replaces l2net (the verifier) OR changes pack content on an installed
  system MUST refresh /etc/l2/pack-hash atomically with the change
  (today: l2install re-run; future l2pm packages shipping l2net or
  modules/firmware need the same hook) — otherwise fail-closed bricks
  hw/wifi on upgrade. Deployment ordering is naturally safe: old ISOs
  carry old l2net in immutable initramfs (unaffected); P2 shell-side and
  build-side ship in the SAME release; installed updates go through the
  pin-refreshing path.

## Verification chain — L2-ea l2net/l2install shell side
1. L2-LIVE marker required (P1; necessary but forgeable — not the anchor).
2. Read discovered pack's `pack-manifest`; sha256(manifest) must equal the
   pin entry for that packname. Missing pin/entry ⇒ refuse.
3. Per-file verification — UNIFORM RULE (A1, L2-ea amendment, ACCEPTED:
   the lead's split-by-subtree counter-proposal was UNSAFE — l2net boot
   load_modules probes ALL modaliases, so with wifi HW present the wifi
   pack activates AT BOOT and drivers request firmware during probe,
   BEFORE any wifi-start; hw-pack NIC drivers (r8169) likewise request
   rtl_nic firmware at boot probe): verify ALL files of a pack
   (lib/modules + bin + lib/firmware) at that pack's FIRST activation,
   whatever triggers it (boot load_modules, wifi-start, l2net modules,
   openssl launcher, l2install tool use). Verification runs in use_pack
   BEFORE any modprobe/exec/firmware-path exposure. Cost: hw/install
   packs small at boot; wifi pack ~1 s one-time when wifi HW present
   (measured per mission-70). /run verification cache keyed by manifest
   hash for same-boot re-runs (tmpfs, per-boot, root-owned). Binaries
   hashed before exec (subsumed, kept as explicit invariant).
   A2 PARSE HARDENING (ACCEPTED + lead extension): the VERIFIER rejects
   malformed manifests itself — absolute paths, '..' components,
   duplicate relpaths (shadowing), non-hex/short hashes (exactly 64
   lowercase hex), CRLF, wrong line format — fail-closed with log;
   build-side determinism is NOT a security boundary (a crafted medium
   controls manifest bytes). EXTENSION: pack tree must contain only
   regular files + directories — reject symlinks/devices/FIFOs upfront
   (busybox sha256sum follows symlinks; rejection closes traversal).
4. FAIL-CLOSED with log on ANY mismatch; never fall back to unverified use.

## Edge cases (documented behavior)
- Installed kernel upgrade without modules: old root pin vs new medium
  manifest ⇒ mismatch ⇒ fail-closed (correct; refresh via l2install).
- Core ISO without packs: empty pin ⇒ any discovered pack refused.
- Rescue/RAM-only boot: initramfs pin applies.
- Attacker possessing genuine pack content: hashes match genuine — no
  attack (pin binds to boot-chain-trusted content, not to a device).
- MODULE_SIG: orthogonal kernel-side defense; NOT required for this
  scheme; stays kernel-lane + user decision (size/key trade-off).
- Upgrade without pin refresh ⇒ fail-closed refusal (A3 invariant;
  mission-70 regression case). Old ISO + old initramfs l2net unaffected
  by P2 (immutable); P2 ships build-side + shell-side in one release.

## Mission-70 regression requirements (ours, after P2 lands)
- Crafted medium with l2/hw but wrong/missing manifest ⇒ refused + logged.
- Genuine medium + genuine pin ⇒ pack activates; boot-time cost measured
  (target: negligible on core ISO, hw pack module hashing ~0.1–0.3 s).
- Installed boot with /lib/modules/$KREL removed ⇒ fall-through refused
  (root pin), no medium modules loaded.
- wifi-start with tampered firmware ⇒ refused before kernel firmware load.
- Reproducibility: double ISO build with pin injection ⇒ byte-identical.
- Wifi-HW-present boot: wifi pack activates at boot via load_modules;
  full-pack verification completes BEFORE driver firmware requests;
  boot cost measured (~1 s target for ~40 MiB). (A1)
- Malformed-manifest refusal: '..' component, duplicate relpath, absolute
  path, short/non-hex hash, CRLF, symlink/non-regular file in pack tree. (A2)
- Upgrade-without-pin-refresh refusal (A3 invariant documented).

## Open items — RESOLVED (co-sign 22:20, lead acceptance 22:25)
- Per-file scope: A1 uniform full-pack-at-first-activation (replaces both
  earlier proposals). A2 parse hardening + non-regular-file rejection.
  A3 upgrade invariant + deployment ordering. Pin filename/format and
  initramfs-rebuild flow ACKed as drafted. Per-pack manifests YES;
  host-install pin from staging YES. Contract CO-SIGNED; implementation
  queued per sequencing above.