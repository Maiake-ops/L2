# Optional Buildroot packages. L2 still owns init, the kernel, and ISO assembly.
DESKTOP_BR=$BUILD/desktop/buildroot

desktop_packages() (
    # Keep native kernel-build includes and libraries out of the musl cross build.
    unset CPATH LIBRARY_PATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH
    mkdir -p "$BUILD/desktop"
    exec 9>"$BUILD/desktop/build.lock"
    flock 9
    local version archive dir make_bin=make
    version=$(lock_get buildroot_version)
    archive=buildroot-$version.tar.xz
    dir=$SRC/buildroot-$version
    local sha; sha=$(lock_get buildroot_sha256)
    [[ $sha =~ ^[0-9a-f]{64}$ ]] || die "missing Buildroot checksum in sources.lock"
    download "https://buildroot.org/downloads/$archive" "$CACHE/$archive" "$sha" "Buildroot $version"
    if [ ! -f "$dir/Makefile" ]; then tar xf "$CACHE/$archive" -C "$SRC"; fi
    if [ "$DISTRO" = nixos ]; then
        local fhs; fhs=$(nix-build "$L2_ROOT/desktop/fhs.nix" --no-out-link)
        make_bin=$fhs/bin/l2-desktop-make
    fi
    local args=(-C "$dir" "O=$DESKTOP_BR" "BR2_EXTERNAL=$L2_ROOT/desktop"
                "BR2_DL_DIR=$CACHE/buildroot" "BR2_DEFCONFIG=$L2_ROOT/desktop/buildroot.config"
                "BR2_JLEVEL=$JOBS")
    local log=$LOGS/desktop-packages.log
    step "Building desktop package(s): $* (log: $log)"
    "$make_bin" "${args[@]}" defconfig > "$log" 2>&1 || { ui_logtail "$log"; die "Buildroot configuration failed"; }
    # Refresh the merged runtime after rebuilding one package of an existing desktop.
    if [ "$1" != all ] && [ -x "$DESKTOP_BR/target/usr/bin/l2wm" ]; then
        set -- "$@" target-finalize
    fi
    "$make_bin" "${args[@]}" -j"$JOBS" -l"$LOAD" "$@" >> "$log" 2>&1 ||
        { ui_logtail "$log"; die "desktop build failed (log: $log)"; }
)

build_desktop_config() {
    # The graphical initramfs exceeds rcl2's 16 MiB live /tmp cap.
    sed -i '2i export TMPDIR=${TMPDIR:-/dev/shm}' "$ROOTFS/bin/l2install"
    cp -a "$L2_ROOT/desktop/rootfs/." "$ROOTFS/"
    chmod 0755 "$ROOTFS/bin/netsurf" "$ROOTFS/bin/l2menu" 2>/dev/null || true
    mkdir -p "$ROOTFS/var/log"
    chmod 1777 "$ROOTFS/tmp"
    # A passwordless local root login matches L2's live console access.
    sed -i 's/^root:x:/root::/' "$ROOTFS/etc/passwd"
    sed -i 's|^service tty1 .*|service tty1 respawn /bin/getty 0 tty1 linux|' "$ROOTFS/etc/rcl2.conf"
    cat >> "$ROOTFS/etc/profile" <<'PROFILE'
if [ -f /etc/l2/desktop-autostart ] && [ -z "${DISPLAY:-}" ] && [ "$(id -u)" = 0 ] && [ "$(tty)" = /dev/tty1 ]; then startx; fi
PROFILE
    if [ "${L2_DESKTOP_AUTOSTART:-1}" = 1 ]; then touch "$ROOTFS/etc/l2/desktop-autostart"; fi
    printf 'Desktop: log in as root on tty1 to start X11 (l2wm). Super+Return terminal, Super+d app launcher, netsurf browser.\n' >> "$ROOTFS/etc/motd"
}

desktop_rootfs() {
    local ROOTFS=$BUILD/desktop/rootfs PACK=$BUILD/desktop/pack target=$DESKTOP_BR/target bin
    local programs=(Xorg xterm startx xinit xauth mcookie xkbcomp l2wm netsurf-fb)
    for bin in "${programs[@]}"; do
        [ -x "$target/usr/bin/$bin" ] || die "missing compiled $bin; run: l2 build desktop"
    done
    local path
    for path in lib/ld-musl-x86_64.so.1 usr/lib/xorg/modules/drivers/fbdev_drv.so \
                usr/lib/xorg/modules/input/evdev_drv.so usr/lib/xorg/modules/input/synaptics_drv.so \
                usr/share/netsurf/default.css \
                usr/share/X11/xkb/rules/base usr/share/fonts/X11/misc/fonts.dir; do
        [ -e "$target/$path" ] || die "missing desktop runtime file $path; run: l2 build desktop"
    done
    rm -rf "$PACK"
    mkdir -p "$PACK/bin" "$PACK/lib" "$PACK/usr/lib/xorg/modules/drivers" \
             "$PACK/usr/lib/xorg/modules/input" "$PACK/usr/share/X11" \
             "$PACK/usr/share/fonts/X11/misc" "$PACK/usr/share/terminfo"
    for bin in "${programs[@]}"; do
        install -m 0755 "$target/usr/bin/$bin" "$PACK/bin/$bin"
    done
    # Compile tiny static l2audio helper if sources are newer or binary missing.
    if [ ! -x "$OUT/l2audio" ] || [ "$L2_ROOT/l2audio/l2audio.c" -nt "$OUT/l2audio" ]; then
        local cc; cc=$(musl_cc)
        "$cc" -std=c11 -Os -static -ffunction-sections -fdata-sections -Wl,--gc-sections \
            -Wall -Wextra -o "$OUT/l2audio" "$L2_ROOT/l2audio/l2audio.c" -lm \
            || die "l2audio build failed"
        have strip && strip -s "$OUT/l2audio" 2>/dev/null || true
    fi
    install -m 0755 "$OUT/l2audio" "$PACK/bin/l2audio"
    # Copy only the Xorg modules actually used (fbdev + evdev/synaptics).
    cp -a "$target/usr/lib/xorg/modules/drivers/fbdev_drv.so" "$PACK/usr/lib/xorg/modules/drivers/"
    cp -a "$target/usr/lib/xorg/modules/input/evdev_drv.so" "$PACK/usr/lib/xorg/modules/input/"
    cp -a "$target/usr/lib/xorg/modules/input/synaptics_drv.so" "$PACK/usr/lib/xorg/modules/input/"
    for mod in libshadow.so libfbdevhw.so; do
        [ -e "$target/usr/lib/xorg/modules/$mod" ] && cp -a "$target/usr/lib/xorg/modules/$mod" "$PACK/usr/lib/xorg/modules/"
    done
    [ -e "$target/usr/lib/xorg/protocol.txt" ] && cp -a "$target/usr/lib/xorg/protocol.txt" "$PACK/usr/lib/xorg/"
    [ -d "$target/usr/lib/X11" ] && cp -a "$target/usr/lib/X11" "$PACK/usr/lib/"
    # Copy only the transitive NEEDED shared libraries (drops unused libstdc++, libgomp, etc.).
    python3 - "$target" "$PACK" <<'PY'
import os, shutil, subprocess, sys
target, rootfs = sys.argv[1], sys.argv[2]
seeds = [
    os.path.join(rootfs, "bin", b)
    for b in ("Xorg", "xterm", "xinit", "xauth", "mcookie", "xkbcomp", "l2wm", "netsurf-fb")
] + [
    os.path.join(rootfs, "usr/lib/xorg/modules/drivers/fbdev_drv.so"),
    os.path.join(rootfs, "usr/lib/xorg/modules/input/evdev_drv.so"),
    os.path.join(rootfs, "usr/lib/xorg/modules/input/synaptics_drv.so"),
    os.path.join(rootfs, "usr/lib/xorg/modules/libshadow.so"),
    os.path.join(rootfs, "usr/lib/xorg/modules/libfbdevhw.so"),
]
def needed(path):
    out = subprocess.check_output(["readelf", "-d", path], text=True, stderr=subprocess.DEVNULL)
    return [l.split("[")[1].split("]")[0] for l in out.splitlines() if "(NEEDED)" in l]
def copy_entry(rel):
    src = os.path.join(target, rel)
    dst = os.path.join(rootfs, rel)
    if os.path.lexists(dst) or not os.path.lexists(src):
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.islink(src):
        linkto = os.readlink(src)
        os.symlink(linkto, dst)
        resolved = os.path.normpath(os.path.join(os.path.dirname(rel), linkto)) if not os.path.isabs(linkto) else linkto.lstrip("/")
        copy_entry(resolved)
    else:
        shutil.copy2(src, dst)
def find_rel(name):
    for d in ("lib", "usr/lib"):
        rel = os.path.join(d, name)
        if os.path.lexists(os.path.join(target, rel)):
            return rel
    return None
copy_entry("lib/ld-musl-x86_64.so.1")
seen = set()
queue = [s for s in seeds if os.path.exists(s)]
while queue:
    p = queue.pop()
    rp = os.path.realpath(p)
    if rp in seen:
        continue
    seen.add(rp)
    for lib in needed(rp):
        rel = find_rel(lib)
        if rel:
            copy_entry(rel)
            queue.append(os.path.join(target, rel))
PY
    # Strip copied desktop ELF executables (notably xterm installed 0555 in Buildroot) and shared libs.
    if have strip; then
        for bin in "${programs[@]}"; do
            strip -s "$PACK/bin/$bin" 2>/dev/null || true
        done
        find "$PACK/usr/lib" -type f -exec strip --strip-unneeded {} + 2>/dev/null || true
    fi
    # Trimmed X11 data: keep XKB data required for evdev/pc105/us, drop other layouts/geometries/XML/locales.
    cp -a "$target/usr/share/X11/." "$PACK/usr/share/X11/"
    rm -rf "$PACK/usr/share/X11/xkb/symbols/"*_vndr \
           "$PACK/usr/share/X11/xkb/rules/"*.xml \
           "$PACK/usr/share/X11/xkb/rules/"*.lst \
           "$PACK/usr/share/X11/xkb/rules/xfree98" \
           "$PACK/usr/share/X11/xkb/rules/xkb.dtd"
    find "$PACK/usr/share/X11/xkb/symbols" -mindepth 1 -maxdepth 1 ! \( \
        -name pc -o -name us -o -name inet -o -name srvr_ctrl -o -name keypad -o \
        -name altwin -o -name capslock -o -name compose -o -name ctrl -o -name empty -o \
        -name eurosign -o -name group -o -name kpdl -o -name latin -o -name level2 -o \
        -name level3 -o -name level5 -o -name nbsp -o -name olpc -o -name parens -o \
        -name RupeeSign -o -name shift -o -name terminate -o -name typo -o -name README \) -exec rm -rf {} + 2>/dev/null || true
    find "$PACK/usr/share/X11/xkb/geometry" -mindepth 1 -maxdepth 1 ! \( -name pc -o -name README \) -exec rm -rf {} + 2>/dev/null || true
    find "$PACK/usr/share/X11/xkb/keycodes" -mindepth 1 -maxdepth 1 ! \( -name evdev -o -name aliases -o -name README \) -exec rm -rf {} + 2>/dev/null || true
    [ -f "$PACK/usr/share/X11/xkb/rules/evdev" ] && ln -sf evdev "$PACK/usr/share/X11/xkb/rules/base"
    rm -rf "$PACK/usr/share/X11/xkb/compiled"
    ln -s /tmp "$PACK/usr/share/X11/xkb/compiled"
    if [ -d "$PACK/usr/share/X11/locale" ]; then
        find "$PACK/usr/share/X11/locale" -mindepth 1 -maxdepth 1 ! \( \
            -name C -o -name iso8859-1 -o -name en_US.UTF-8 -o \
            -name locale.alias -o -name locale.dir -o -name compose.dir \) -exec rm -rf {} + 2>/dev/null || true
    fi
    # Minimal X11 bitmap fonts (fixed + cursor + common sizes); omit large Unicode 6x13.pcf.gz.
    local f
    for f in cursor.pcf.gz 6x13-ISO8859-1.pcf.gz 6x13B-ISO8859-1.pcf.gz \
             7x13-ISO8859-1.pcf.gz 8x13-ISO8859-1.pcf.gz 9x15-ISO8859-1.pcf.gz 10x20-ISO8859-1.pcf.gz; do
        [ -f "$target/usr/share/fonts/X11/misc/$f" ] && \
            install -m 0644 "$target/usr/share/fonts/X11/misc/$f" "$PACK/usr/share/fonts/X11/misc/$f"
    done
    [ -f "$target/usr/share/fonts/X11/misc/fonts.alias" ] && \
        install -m 0644 "$target/usr/share/fonts/X11/misc/fonts.alias" "$PACK/usr/share/fonts/X11/misc/fonts.alias"
    awk 'NR == 1 { next } { f = $1; if (system("test -f \"'"$PACK"'/usr/share/fonts/X11/misc/" f "\"") == 0) lines[++n] = $0 }
         END { print n + 0; for (i = 1; i <= n; i++) print lines[i] }' \
        "$target/usr/share/fonts/X11/misc/fonts.dir" > "$PACK/usr/share/fonts/X11/misc/fonts.dir"
    # Minimal terminfo set for xterm and Linux console.
    for f in a/ansi d/dumb l/linux v/vt100 v/vt220 x/xterm x/xterm-color x/xterm-256color; do
        if [ -e "$target/usr/share/terminfo/$f" ]; then
            mkdir -p "$PACK/usr/share/terminfo/$(dirname "$f")"
            cp -a "$target/usr/share/terminfo/$f" "$PACK/usr/share/terminfo/$f"
        fi
    done
    # NetSurf resources and TLS CA bundle (kept on the ISO medium).
    cp -a "$target/usr/share/netsurf" "$PACK/usr/share/"
    if [ -f "$target/etc/ssl/certs/ca-certificates.crt" ]; then
        cp -a "$target/etc/ssl/certs/ca-certificates.crt" "$PACK/usr/share/netsurf/ca-bundle.txt"
    fi
    find "$PACK" -exec touch -h -d "@$EPOCH" {} +

    # Keep the desktop payload on the ISO medium (l2/desktop) instead of tmpfs/Shmem.
    build_rootfs
    for bin in "${programs[@]}" l2audio; do
        ln -sf "/media/l2/l2/desktop/bin/$bin" "$ROOTFS/bin/$bin"
    done
    mkdir -p "$ROOTFS/lib"
    for f in "$PACK/lib/"*; do
        ln -sf "/media/l2/l2/desktop/lib/${f##*/}" "$ROOTFS/lib/${f##*/}"
    done
    rm -rf "$ROOTFS/usr/lib" "$ROOTFS/usr/share"
    ln -sf /media/l2/l2/desktop/usr/lib "$ROOTFS/usr/lib"
    ln -sf /media/l2/l2/desktop/usr/share "$ROOTFS/usr/share"
    build_desktop_config
    find "$ROOTFS" -exec touch -h -d "@$EPOCH" {} +
    build_initramfs "$OUT/initramfs-l2-desktop"
}

desktop_iso() (
    mkdir -p "$BUILD/desktop"
    exec 9>"$BUILD/desktop/build.lock"
    flock 9
    trap 'rm -f "$OUT/l2-desktop.iso.part"' EXIT
    desktop_rootfs
    local wifi=0
    [ ! -d "$BUILD/wifi" ] || wifi=1
    _make_iso "$OUT/l2-desktop.iso.part" "$L2_DESKTOP_ISO_LIMIT_MB" desktop "$wifi" "$OUT/initramfs-l2-desktop"
    mv -f "$OUT/l2-desktop.iso.part" "$OUT/l2-desktop.iso"
)

build_desktop() {
    local c krel limver file
    build_musl
    krel=$(cat "$OUT/kernelrelease" 2>/dev/null || true)
    if [ ! -s "$OUT/vmlinuz-l2" ] || { [ ! -d "$BUILD/hw" ] && [ ! -d "$BUILD/modroot/lib/modules/${krel:-missing}" ]; }; then
        build_kernel
    fi
    for c in busybox dash rcl2 l2pm l2wifi; do [ -x "$OUT/$c" ] || build_one "$c"; done
    for file in bin/limine share/limine/{limine-bios.sys,limine-bios-cd.bin,limine-uefi-cd.bin,BOOTX64.EFI}; do
        if [ ! -s "$OUT/limine/$file" ]; then build_limine; break; fi
    done
    [ -d "$BUILD/hw" ] || build_hwpack
    for file in bin/{limine,sgdisk,mke2fs,mkfs.fat} etc/{cmdline,limine-installed.conf}; do
        [ ! -s "$BUILD/installtools/$file" ] || continue
        limver=$(lock_get limine_version)
        [ -f "$SRC/limine-$limver/bin/limine.c" ] || build_limine
        [ -x "$OUT/l2gpt" ] || build_l2gpt
        [ -x "$OUT/mke2fs" ] || build_e2fsprogs
        [ -x "$OUT/mkfs.fat" ] || build_dosfstools
        build_installpack
        break
    done
    desktop_packages all
    build_iso desktop
}
