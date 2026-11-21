# L2 - tiny Linux for tools

L2 is a small, source-built Linux system: **rcl2** as PID 1, **musl** userland,
**busybox** for the core tools, **dash** as `/bin/sh` for POSIX scripts, and
**l2pm** as a content-addressed, reproducible package manager.

Everything is driven by the `l2` gum TUI (or the same commands non-interactively).

**0.4** drops minibox/sash in favour of a single busybox toolset, fixes the
busybox/wpa_supplicant/OpenSSL builds, and widens real-hardware support (I2C HID
touchpads/keyboards, eMMC/SD, exFAT, UEFI variables, old USB controllers). See
"0.4 changes" below. **0.2** added a themed TUI, real-hardware boot support, a
wifi stack that costs nothing at idle, a kernel settings menu, a rewritten
installer and QEMU tests.

```
 l2                 gum TUI: deps, component selection, kernel, ISO, l2pm
 l2fetch            tiny fetch tool, no dependencies
 l2install          install L2 to disk / build ISO / install tools on any distro
 l2pm/              package manager (C, static) + recipes
 config/            kernel fragment, rcl2.conf, limine.conf, limine-installed.conf
 rcl2/              your existing init (used as-is)
 sources.lock       pinned kernel/musl/dash/busybox versions + sha256
```

## Quick start

Clone only the Linux source branch (excluding website branch history):

```sh
git clone --single-branch --branch main https://github.com/Maiake-ops/L2.git
cd L2
```

The project website is published separately from the `gh-pages` branch.

```sh
# interactive (gum)
./l2

# or fully non-interactive:
./l2 deps install        # Debian, Arch, Gentoo, NixOS, Alpine, Fedora, Void
./l2 build all           # musl + kernel + busybox + dash + rcl2 + l2pm + limine
                         # rootfs + initramfs + ISO, -j16 per program
./l2 doctor              # tool table, artifact sizes, l2pm RAM check

# installable artifacts
./l2 iso                 # ./build/out/l2.iso (limine, BIOS+UEFI), gated at <10 MiB
./l2install shared       # l2pm, l2fetch, l2install -> /usr/local (any distro)
./l2install disk /dev/sdX   # destructive BIOS/UEFI install of L2
```

`l2 build all` compiles the kernel, so allow time and ~2 GiB free disk for
sources/build trees. `l2 build musl`, `l2 build kernel`, `l2 build busybox`,
`l2 build rcl2`, `l2 build l2pm`, `l2 build rootfs`, `l2 build initramfs`,
`l2 build iso` build individual pieces. The interactive menu has the same
selection.

## Desktop Edition

`./l2 build desktop` builds the optional X11 desktop and writes
`build/out/l2-desktop.iso`. It uses the same kernel, Limine bootloader,
installer and available hardware or Wi-Fi pack as the console editions.
`./l2 iso desktop` rebuilds the desktop rootfs and ISO from compiled
components. Desktop staging lives in `build/desktop/` and its initramfs
is `build/out/initramfs-l2-desktop`. The desktop ISO budget defaults to
128 MiB and can be set with `L2_DESKTOP_ISO_LIMIT_MB`.

The source lock pins Buildroot. Its recipes and archive hashes pin the
x86-64 musl toolchain, Xorg, fbdev and evdev drivers, xinit, Xlib, bitmap
fonts and xterm. The first build needs network access. Buildroot uses its
download cache under `build/cache/buildroot/`. On NixOS the builder runs
make inside a Nix FHS environment. On other Linux hosts it uses GNU make
and the standard Buildroot host tools.

At the tty1 login prompt enter `root` (empty password on live media), then
run `startx`. The serial console retains the standard L2 root shell.
Xorg uses the firmware framebuffer and detects connected evdev keyboards
and pointers at session startup. The default xinit session opens xterm
under l2wm. An executable `~/.xinitrc` overrides that session.

| Shortcut | Action |
|---|---|
| Super+Return | Open terminal (`L2WM_TERMINAL`, `terminal` in config, or `xterm`) |
| Super+j / Super+k | Focus the next / previous window |
| Super+Shift+j / Super+Shift+k | Swap focused window with next / previous tiled window |
| Super+m / Super+Shift+Return | Promote or swap focused window with master |
| Super+h / Super+l | Shrink / grow master area by 5% (10–90%) |
| Super+f / Super+Space | Toggle master-stack tiling and monocle layout |
| Super+t | Toggle floating mode for the focused window |
| Super+Left/Right Drag | Move / resize a floating window |
| Super+1..9 / Super+Shift+1..9 | Switch workspace / move focused window to workspace |
| Super+q | Close the focused window |
| Super+r (or `SIGHUP`) | Reload `/etc/l2/l2wm.conf` (and `~/.config/l2wm/l2wm.conf`) and retile |
| Super+Escape | End the X session and return to the console |

Set `L2_DESKTOP_AUTOSTART=1` when packaging to start X after tty1 login,
or create `/etc/l2/desktop-autostart` on an installed desktop. Run
`poweroff` for a clean rcl2 shutdown. To exercise the graphical session
headlessly, run `python3 tools/test-desktop.py build/out/l2-desktop.iso`
and repeat with `--uefi` for OVMF.

## What gets built

| component | source | notes |
|---|---|---|
| Linux kernel | latest stable in `sources.lock` (7.3.3 at the time of writing) | `tinyconfig` + `config/kernel.fragment`; storage/net/input built in, wifi drivers are modules |
| musl | pinned 1.2.5 | static libc + generated `musl-gcc` wrapper (no distro musl-gcc required) |
| busybox | pinned 1.36.1 | the core toolset (ls, cat, cp, grep, sed, awk, vi, tar, …) plus udhcpc, ip, modprobe, mount, fdisk; static, no shell |
| dash | pinned 0.5.12 | `/bin/sh` for scripts and rcl2's default shell |
| rcl2 | `./rcl2` (your tree) | static, no libc, ~26 KiB, x86_64, PID 1 |
| limine | pinned 9.6.7 | hybrid BIOS+UEFI bootloader (ISO and installed disk) |
| l2pm | `l2pm/l2pm.c` | static C, built-in sha256 + tar, content-addressed store |
| l2fetch | this repo | POSIX sh, reads `/proc`, no dependencies |
| l2install | this repo | POSIX sh, shared install / ISO / disk / directory |

### Why busybox and dash?

busybox supplies every core utility as a single static multi-call binary; each
tool in `/bin` is a symlink to it.  Its own shells are disabled (`SH_IS_NONE`),
so `dash` is the only shell and is installed as `/bin/sh` (rcl2's compiled-in
default shell is `/bin/dash`).  This keeps `#!/bin/sh` scripts, `l2fetch`,
`l2install` and the installer working with one small, well-understood toolset.
`poweroff`/`reboot`/`halt` are tiny wrappers that signal rcl2 (PID 1) for a clean
shutdown, rather than busybox's applets (whose signal mapping differs).

## Reproducible builds

Reproducibility is a build property, not a promise:

- `sources.lock` pins versions and sha256 hashes. `l2 update` resolves the
  newest stable kernel, musl, dash, and limine, then rewrites the lock
  (network + disk space required).
- `SOURCE_DATE_EPOCH` is fixed from the lock (`epoch=`), defaulting to
  2026-01-01. All component builds export it.
- l2pm archives are **ustar** with entries sorted bytewise, `uid/gid=0`,
  `root/root`, fixed mtime, no device nodes, and no absolute paths.
- compiler flags use `-ffile-prefix-map` / `-fdebug-prefix-map` and fixed
  optimisation flags; the kernel gets fixed `KBUILD_BUILD_TIMESTAMP`,
  `KBUILD_BUILD_USER` and `KBUILD_BUILD_HOST`.
- the initramfs is a sorted `newc` cpio archive with `--owner=0:0` and
  `gzip -n` (no filename/time in the gzip header).
- ISO files are touched to the epoch; xorriso reads `SOURCE_DATE_EPOCH` and
  writes fixed volume/file dates. Limine's MBR disk ID is seeded from the epoch
  and the four ID bytes are pinned after `limine bios-install`, so rebuilds are
  byte-identical.
- `l2pm build <recipe> --check` builds twice, hashes both archives, and fails
  if they differ. `l2 pm build l2pm --check` is a quick end-to-end check.

To prove it on your machine:

```sh
./l2 build all
sha256sum build/out/vmlinuz-l2 build/out/initramfs-l2 build/out/l2.iso
./l2 build all             # second build, same toolchain + lock
sha256sum build/out/vmlinuz-l2 build/out/initramfs-l2 build/out/l2.iso
```

## l2pm - package manager

l2pm is a static C program with no runtime dependency on `tar`, `sha256sum`,
`gzip` or even a shell. Innovation points, in the spirit of Nix:

- **content-addressed store**: `/l2/store/<sha256>-<name>-<version>`
- **immutable packages**: a package is installed by extracting a verified
  `.l2p` archive into the store; existing store paths are never overwritten
- **profiles**: `/l2/var/profiles/default` is a tree of symlinks into the store
  (`/usr/bin` in the L2 image points here)
- **atomic install**: extract to a sibling temp directory, then `rename(2)`
- **garbage collection**: remove DB entries, then `l2pm gc` deletes store paths
  no longer referenced by any installed package
- **reproducible package format**: deterministic ustar, sorted, fixed metadata
- **tiny**: ~67 KiB static binary, ~130 KiB resident on self-test, far below the
  10 MiB RAM and 10 MiB storage budgets

```
l2pm build recipes/l2pm.recipe --check
l2pm install build/out/l2pm-0.1.0.l2p
l2pm add recipes/l2fetch.recipe
l2pm list
l2pm info l2fetch
l2pm verify
l2pm remove l2fetch
l2pm gc
l2pm selftest          # prints RSS and budget
```

`L2PM_ROOT` prefixes every path (used by `l2 pm` under `build/pmroot`);
`L2PM_RECIPES` points at a recipe directory. `l2 pm ...` wires both and uses the
musl-built binary automatically.

### Recipe format and the 4-step pipeline

One file, `key = value`. Builds run in four numbered steps:

1. **repo** — obtain the source: a git clone for `repo=` recipes, or the
   recipe itself when it comes from a remote recipe repository
2. **fetch/extract** — download + verify the tarball (`source=`/`sha256=`),
   or export the pinned git worktree (`.git` removed, mtimes normalized to
   `SOURCE_DATE_EPOCH`)
3. **compile** — the `compile=` command (optional)
4. **install** — the `install=` command stages files into `$DESTDIR`; the
   archive → store → profile flow is unchanged

Legacy recipes with a single `build=` command keep working exactly as before
(`build=` is compile+install combined). Mixing `build=` with `compile=`/
`install=` is an error.

```ini
name = hello
version = 1.0
epoch = 1767225600
deps =

# tarball source (step 2 verifies the pinned sha256):
source = https://example.org/hello-1.0.tar.gz
sha256 = <pinned hash>
source_dir = hello-1.0

# ...or a git source: the commit hash IS the pin (replaces sha256).
# repo = https://example.org/hello.git
# ref = <full commit hash>        # tags/branches work but warn: not reproducible

compile = make -j"$JOBS"
install = make DESTDIR="$DESTDIR" PREFIX=/usr install
```

All commands run with `/bin/sh -e -u -c` in the extracted source directory,
with `DESTDIR`, `PREFIX=/usr`, `SRC`, `SOURCE_DATE_EPOCH`, `JOBS`, `CC`,
`TZ=UTC`, `LC_ALL=C` and a fixed umask. A package is whatever the recipe
stages under `$DESTDIR`. `git` is required for `repo=` sources and git recipe
repositories (it is in every `l2 deps` list).

### Remote recipe repositories

`l2pm add <name>` / `l2pm build <name>` resolve bare names locally first
(CWD, `L2PM_RECIPES`, `<root>/l2/recipes`), then in `L2PM_RECIPE_REPO`:

- **git URL** (`git://`, `ssh://`, `git@…`, or ending in `.git`): cloned into
  `<root>/l2/var/recipe-repo`; pin it with `L2PM_RECIPE_REPO_REF=<commit>`.
- **raw base URL** (e.g. `https://host/recipes`): downloads
  `<base>/<name>.recipe` with curl/wget.

Fetched recipes are cached and keep working offline. **A fetched recipe
executes arbitrary shell**: l2pm shows it and refuses to run it until you
confirm interactively or pass `--trust`. Trusted recipe content hashes are
recorded in `<root>/l2/var/trusted-recipes`; a changed recipe must be
trusted again. Local recipes never need the gate.

## l2fetch

```
l2fetch [--no-color] [--no-logo]
```

Prints OS, kernel, uptime, package count, shell, terminal, CPU, RAM, init,
L2 store usage. It is POSIX sh and only reads `/proc`; it works on any Linux,
not just L2.

## l2install

```
l2install shared              # install l2pm, l2fetch, l2install + recipes
l2install iso [out.iso]       # build the tiny ISO via ./l2
l2install disk /dev/sdX       # erase a disk, BIOS or UEFI
l2install dir /mnt/target     # copy just the rootfs (no bootloader)
l2install menu                # gum TUI
```

`shared` is the "installable on all machines" path: it only needs `install`
and a POSIX shell. `disk` is destructive and asks you to type the device name
unless `--yes` is passed. It creates a hybrid GPT layout (1 MiB BIOS stage-2
partition + 256 MiB ESP + ext4 root), installs **Limine** for both BIOS
(stage 1 in the MBR, stage 2 in partition 1) and UEFI (`/EFI/BOOT/BOOTX64.EFI`), and writes `/etc/fstab`,
`/etc/l2/installed`, the kernel/initramfs, and a `guid(PARTUUID)` based
`limine.conf`. The kernel and rcl2 are x86_64; `shared` and `l2fetch` are
architecture-neutral.

## Size and memory budgets

Measured in this tree (musl static, x86_64):

| artifact | size |
|---|---:|
| l2pm binary | ~67 KiB |
| l2pm resident (self-test, VmHWM) | ~130 KiB |
| busybox (core toolset, static) | ~520 KiB |
| dash | ~180 KiB |
| rcl2 | 26,808 bytes |
| rootfs (before cpio/gzip) | ~1 MiB |
| initramfs (gzip) | ~330 KiB |
| kernel (bzImage, tinyconfig + L2 fragment) | ~3.5 MiB |
| ISO (limine, BIOS+UEFI hybrid) | <10 MiB (hard gate) |

Run `l2 doctor` for the exact sizes on your machine; the table above is indicative.

For QEMU boot reliability, PID 1 reaping and service recovery, memory trends,
bounded stress, persistent filesystem recovery, and optional 24–72 hour soak
tests, run `python3 tools/test-stability.py --iso build/out/l2.iso`.
See [the stability test guide](tools/STABILITY.md) for release runs and reports.

The kernel and ISO sizes are reported by `l2 doctor` and `l2 iso`; the ISO is
hard-gated at **10 MiB** (`L2_ISO_LIMIT_MB=10`). The ISO is a **Limine hybrid**
image (BIOS El Torito + UEFI El Torito + protective MBR), so the same file boots
on BIOS and UEFI machines and is USB-writable. If the kernel/ISO exceeds the
gate, the build fails instead of silently producing a larger image;
`L2_ALLOW_BIG_ISO=1` overrides it.

The kernel fragment is deliberately small: no debug info, no KALLSYMS, and only
the drivers needed to boot and use real machines — ext4/exFAT/initramfs,
virtio/AHCI/NVMe/eMMC/USB storage, PS/2 + USB + I2C HID input, VESA/EFI fb,
ACPI poweroff, wired ethernet, and wifi as modules. `make olddefconfig` drops
symbols that a future kernel no longer has; `l2 doctor` and
`build/logs/kernel-dropped.txt` list them so a silent loss of NVMe/HID/wifi is
visible.

## Distribution dependencies

`l2 deps install` installs build tools per distro. `l2 deps check` prints a
table. The important pieces: clang/lld/rustc (rcl2), a host C compiler + make +
bc/bison/flex + libelf headers (kernel), xz/cpio/gzip, **nasm + lld + llvm**
(for Limine), xorriso/mtools (ISO), sgdisk + dosfstools for disk install,
curl/git/sha256, and musl (or the generated musl sysroot from
`l2 build musl`).

NixOS users get `nix profile add` for the packages. The kernel builder adds
`~/.nix-profile/include`, `~/.nix-profile/lib` and the pkg-config directories to
`CPATH`/`LIBRARY_PATH`/`PKG_CONFIG_PATH`, so the `elfutils.dev` headers needed by
objtool are found without a `nix-shell`. The generated `build/bin/musl-gcc`
wrapper is self-contained and works even where the distro has no `musl-gcc`
package; it also handles the Nix gcc wrapper's `-latomic_asneeded` quirk.

## Notes / limits

- rcl2 is x86_64 only (raw syscalls and asm). The builder can cross-build the
  userland elsewhere, but `l2install disk` and the kernel target are x86_64.
- Networking is brought up by `/bin/l2net` (see 0.2 changes); nothing is loaded
  or started for hardware that is not present.
- l2pm archives are hashed and pinned but not cryptographically signed.
  Add a detached signature/sidecar if you distribute packages over a network.
- The store path encodes the hash of the `.l2p` archive; installing the same
  archive twice is idempotent.
- busybox provides the core tools but no shell; `dash` is `/bin/sh` (already
  wired). `poweroff`/`reboot`/`halt` signal rcl2 for a clean shutdown.
- The kernel build needs libelf headers (`gelf.h`) for objtool; that package is
  in the dependency lists.
- Limine is built from its release tarball (not the git tree), so no autotools
  are required. It still needs `nasm`, `ld.lld`, `llvm-objcopy` and `mtools`.
- The installed Limine config addresses the root filesystem with
  `guid(PARTUUID)`, so the same file is valid under BIOS and UEFI.


## 0.2 changes

### TUI
`lib/l2ui.sh` is shared by `l2` and `l2install`: L2 gradient logo, status
dashboard with an ISO budget bar, themed gum menus/prompts, and a build ticker
(spinner, elapsed time, last log line) instead of a silent screen. Without gum
you get the same layout in plain ANSI; on the real console (`TERM=linux`) or a
non-UTF-8 locale every glyph falls back to ASCII. `NO_COLOR`, `L2_PLAIN`,
`L2_ASCII` are honoured.

### Kernel settings
`l2 kernel` (or menu entry) edits `config/kernel.local`, merged after
`config/kernel.fragment`: Wi-Fi families, IPv6, USB ethernet, VM-guest tweaks,
`NR_CPUS`, or any `CONFIG_X=v`. `l2 kernel set CONFIG_HZ_250=y`, `show`, `reset`.
After `olddefconfig` the build lists fragment symbols the kernel dropped
(`build/logs/kernel-dropped.txt`) and `l2 doctor` re-checks NVMe/AHCI/USB/HID/
PS-2/EFI-fb/ACPI/wifi in the built `.config`.

### Booting on real machines
* Installed systems no longer run from the initramfs. The normal Limine entry
  has no `module_path`; the kernel mounts the ext4 root by `PARTUUID` with
  `rootwait` (the old entry silently ran the live image from RAM).
* Kernel fragment now has NVMe, PS/2 (i8042), USB HID, evdev, SR/ATA-SFF, UAS,
  r8169/igc, USB ethernet, ACPI battery/AC/button/idle, RTC, and `SUSPEND` so
  ACPI poweroff works on real firmware. Console order is `console=ttyS0 console=tty0`
  (tty0 last, so init talks to the screen). The serial getty only starts if the
  UART exists. Recovery entries: recovery shell, RAM-only rescue, 1 GiB RAM cap.
* Secure Boot must be off (Limine is unsigned).

### Idle RAM at any RAM size
`config/cmdline` shrinks the size-scaled hash tables (dentry, inode, mount, TCP,
UDP); the kernel uses `SLUB_TINY`, `BASE_SMALL`, a 16 KiB printk ring, no NUMA/
THP/KSM/swap. Installed systems keep no rootfs in RAM. Wifi drivers are modules.
`l2 ram --sizes 256M,1G,4G,16G --budget 10240` boots the image under QEMU at each
size and prints `used` (MemTotal - MemAvailable) and `reserved`.
Be aware of the physics: the kernel's struct page array is about 1.5% of
installed RAM (16 GiB -> ~256 MiB), invisible to `free`. No tuning removes it;
`mem=1G` (boot menu entry) caps it at ~16 MiB if you want a hard bound.

### Wi-Fi
* Kernel: cfg80211/mac80211 and drivers (Intel iwlwifi, MediaTek mt7921e/u and
  mt7925e, Realtek rtw88/rtw89/rtl8xxxu, Atheros ath9k/10k/11k/12k, Broadcom
  brcmfmac) are **modules**. `l2 build kernel` also stages modules + depmod.
* Userland: `l2 build wpa` (static libnl + wpa_supplicant, WPA2/WPA3-PSK, open),
  `l2 build busybox` (udhcpc, ip, modprobe, mount, fdisk), `l2 build l2wifi`.
* Firmware: `l2 build firmware` picks blobs per family from
  `config/wifi-firmware.list` out of linux-firmware (or `L2_FIRMWARE_SRC=/lib/firmware`),
  keeps the two newest iwlwifi revisions, xz-compresses.
* `l2 build wifipack` assembles modules + firmware + wpa tools. The **core ISO**
  (<10 MiB) does not contain it. `l2-wifi.iso` carries it under `l2/wifi` and
  `l2net` mounts the boot medium read-only, so nothing is copied into RAM.
  `l2install` copies the pack onto the installed root.
* Use: `l2wifi` (scan, pick, connect), `l2wifi status|scan|connect SSID|list|forget|country XX`.
  Saved networks persist in `/etc/l2/wpa_supplicant.conf`; wpa_supplicant starts
  at boot only if a network is saved. `l2net status` shows interfaces.
* New sources (busybox, libnl, wpa_supplicant, linux-firmware) are pinned
  on first download into `sources.lock`; verify those hashes once upstream.

### Installer
`l2install` has a disk picker (marks mounted disks, refuses them), a review
panel plus typed confirmation, `-y` (no questions), `--dry-run`, numbered steps
with a log, `image` (loop-device disk image for QEMU or dd), post-install
verification, optional `--efi-entry`, `--hostname`, `--efi-size`, `--no-wifi`,
and carries saved wifi networks over. Options go BEFORE the command, e.g.
`l2install -y --no-wifi disk /dev/sda`.

Disk install works BOTH from a normal Linux host (uses host sgdisk/mkfs) AND
from inside the booted L2 live system. For the live case the tools ride on the
medium in the `l2/install` pack (zero idle RAM): `l2gpt` (a tiny C partitioner
with an sgdisk-compatible CLI, byte-compatible GPT output), `mke2fs` (as
mkfs.ext4), `mkfs.fat`, and a static musl `limine` host tool. `l2install`
detects the live medium, puts the pack's `bin/` on PATH, and uses the medium's
kernel/initramfs as the payload. Build it with `l2 build installpack` (part of
`l2 build all`).

### Tests
`make check` (syntax, l2wifi build, ramtest selftest), `l2 ram`, `l2 bootiso`
(BIOS) and `l2 bootiso --uefi` (OVMF), and `python3 tools/test-live-install.py`
(boots the live ISO, installs to a scratch disk from inside L2, then boots the
installed disk). `l2wifi` was exercised against a mock wpa_supplicant control
socket.

## Layout

```
l2                     gum TUI + build orchestrator (bash)
lib/l2ui.sh            shared UI (gum + ANSI + ASCII)
l2wifi/                wifi manager (C, wpa_supplicant client)
overlay/               l2net, openssl launcher, shutdown, udhcpc script, serial getty
tools/ramtest.py       QEMU RAM sweep and ISO boot test
config/cmdline         RAM-lowering kernel parameters
l2fetch                fetch tool (POSIX sh)
l2install              installer (POSIX sh)
l2pm/l2pm.c            package manager (C, no external runtime tools)
l2pm/Makefile          static build / check / install
l2pm/recipes/          l2pm, rcl2, l2fetch, l2install, l2wifi recipes
config/kernel.fragment tiny kernel config
config/rcl2.conf       L2 services (tty1 + serial console)
config/limine.conf     live ISO boot menu (BIOS + UEFI)
config/limine-installed.conf installed disk menu (guid PARTUUID)
sources.lock           version + sha256 pins
rcl2/                  existing init source, used unchanged
```

## 0.3 changes: OpenSSL (light, off the RAM budget)

`l2 build openssl` builds a **static, musl, `-Os`, trimmed** OpenSSL (no shared
libs, engines, legacy provider, DTLS/SSLv3/TLS1.0-1.1, CMS/OCSP/CT/TS/SRP/PSK,
old and national ciphers, QUIC, kTLS, tests, docs; asm off unless
`L2_OPENSSL_ASM=1`). Options a given OpenSSL release does not know are skipped.
It needs `perl` on the build host (added to `l2 deps`).

Where it lives decides the RAM cost:

| mode | where | idle RAM |
|---|---|---|
| default (`L2_SSL=core`) | `l2/ssl` on the ISO, run from the medium by `/bin/openssl` (a 1 KiB dash launcher that mounts the medium read-only on first use) | 0 KiB |
| `L2_SSL=wifi` | only on `l2-wifi.iso` | 0 KiB |
| `L2_SSL=off` | not shipped | 0 KiB |
| `L2_SSL_EMBED=1` | inside the initramfs | binary size (RAM) |
| installed disk | `l2install` copies the pack onto the ext4 root (`--no-ssl` skips) | 0 KiB (page cache only while running) |

`L2_SSL_CA=1` (host bundle) or `L2_SSL_CA=/path/ca.pem` ships `/etc/ssl/cert.pem`;
it is off by default so builds stay reproducible and small. `OPENSSL_VERSION=x.y.z`
overrides the pin in `sources.lock`; the sha256 is taken from upstream's
`.sha256` file on first download and pinned. Bump the pinned 3.5.x to the
latest patch release (security fixes) before shipping.

The core ISO stays hard-gated at 10 MiB. If openssl pushes it over, the build
fails with a hint (`L2_SSL=wifi` or `off`) instead of producing a bigger image.
Sizes of the openssl binary and the new ISO are **not yet measured** (the 0.3
changes were written without network or a kernel build): run `l2 build openssl`
and `l2 doctor`, which now lists the openssl size.

New boot entry: **cap RAM at 256 MiB**. The kernel's struct-page array is ~1.5%
of installed RAM and no config removes it; `mem=` is the only way to bound it.

## 0.4 changes: one toolset, real hardware, build fixes

### busybox replaces minibox + sash
minibox and its `sash` shell are gone. A single static **busybox** now provides
every core utility (`ls`, `cat`, `cp`, `grep`, `sed`, `awk`, `find`, `vi`, `less`,
`tar`, `gzip`, …) plus `udhcpc`, `ip`, `modprobe`, `mount`, `fdisk`, `blkid`.
`/bin` is a tree of symlinks to it, generated from `busybox --list` so the links
always match the binary. busybox's own shells are disabled; **dash** remains
`/bin/sh`. `poweroff`/`reboot`/`halt` are small wrappers around `rcl2`'s direct
power commands (sync + reboot syscall), which work from any context.

### Build fixes
- **busybox config**: appending `CONFIG_X=y` over an `allnoconfig` `.config` is
  silently ignored by busybox's kconfig (duplicate assignments are dropped), so
  the old build produced a busybox with *no* applets. The merge now replaces
  lines in place, and the build verifies the required applets are really in the
  binary. `CONFIG_BUSYBOX` (the dispatcher) is enabled explicitly.
- **wpa_supplicant**: SAE/WPA3 needs the `crypto_bignum` API, which only the
  OpenSSL/wolfSSL backends implement (the internal crypto does not). wpa is now
  built against the L2 OpenSSL, whose headers/static libs are installed into the
  musl sysroot. The legacy provider stays enabled because `crypto_openssl.c`
  references those algorithms even though WPA2/WPA3-PSK only uses AES/SHA/ECC.
- **OpenSSL**: `no-crmf` (and other internal-only aliases) made `Configure` die
  with "Unsupported options". Options are now filtered against the release's
  real `@disablables` list plus its TLS/DTLS protocol variants.
- **l2wifi**: `make -C l2wifi` uses the project's musl wrapper when present, so
  `make check` works on hosts without a static glibc (e.g. NixOS).
- **installer image**: `tools/mkimage.sh` builds a bootable installed image
  without root (sgdisk + `mkfs.ext4 -d` + mtools); it stops the ext4 short of the
  disk end so the backup GPT is not overwritten (Limine rejects a missing backup).

### Real hardware
The kernel fragment gained: I2C HID + DesignWare I2C + GPIO/pinctrl (modern
laptop touchpads/keyboards), MMC/SDHCI (eMMC/SD), exFAT + NLS_UTF8 (USB sticks),
efivarfs (UEFI variables), OHCI/UHCI (older USB), LEDS_CLASS (rtl8xxxu USB wifi),
ACPI WMI/video + backlight (laptop hotkeys/brightness), and an explicit
`PREEMPT_LAZY` (the low-overhead model 6.13+ x86_64 uses instead of `PREEMPT_NONE`).
`NR_CPUS` is 4 to keep per-CPU memory small (raise with `l2 kernel set`).

### hw pack: real-hardware drivers off the RAM budget
The secondary drivers (I2C HID, exFAT, OHCI/UHCI, ACPI video/WMI, and the physical
NIC drivers e1000/e1000e/igc/r8169/USB-net) are **modules** and are NOT put in the
RAM-resident initramfs. They ride on the medium as `l2/hw` (core ISO) or inside the
wifi pack, and on the installed root. `l2net` loads them by modalias only when the
hardware is present, so they cost nothing at idle.
`l2net`/`openssl` stop searching for a pack as soon as they find the L2 core
medium without one, trimming ~10 s off every core-ISO boot.

### OpenSSL placement (core ISO stays under 10 MiB)
The UEFI El Torito boot image must be Limine's stock 2.8 MiB image (OVMF rejects
smaller/reformatted FAT images), so the core ISO cannot also carry the ~3 MiB
OpenSSL and stay under 10 MiB. Default is therefore `L2_SSL=wifi`: OpenSSL rides
on `l2-wifi.iso` (and the installed root with `--with-ssl`). Set `L2_SSL=core`
(+`L2_ALLOW_BIG_ISO=1`) to also put it on the core ISO.

### Measured budgets (QEMU/KVM, x86_64, 2 vCPUs)
| item | result | budget |
|---|---|---|
| core ISO | ~8.8 MiB | <10 MiB |
| live idle RAM @256 MiB (BIOS / UEFI) | ~9.3 / 8.8 MiB | <10 MiB |
| installed idle RAM @256 MiB | ~8.8 MiB | <10 MiB |
| installed storage (root + ESP) | ~6.6 MiB | <10 MiB |

`l2net` lowers `vm.min_free_kbytes`/`watermark_scale_factor` so the idle reserve
does not inflate the footprint. On big machines the kernel's per-RAM structures
(struct page, etc.) grow; use the "cap RAM at 256 MiB / 1 GiB" boot entries
(`mem=`) to hold the footprint down — that is the only way to bound it.
`tools/ramtest.py` measures via the real ISO (QEMU `-kernel/-initrd` keeps the
initrd in fw_cfg memory and reads ~1 MiB high).
