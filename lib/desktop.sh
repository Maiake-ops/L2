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
    mkdir -p "$ROOTFS/var/log"
    chmod 1777 "$ROOTFS/tmp"
    # A passwordless local root login matches L2's live console access.
    sed -i 's/^root:x:/root::/' "$ROOTFS/etc/passwd"
    sed -i 's|^service tty1 .*|service tty1 respawn /bin/getty 0 tty1 linux|' "$ROOTFS/etc/rcl2.conf"
    cat >> "$ROOTFS/etc/profile" <<'PROFILE'
if [ -f /etc/l2/desktop-autostart ] && [ -z "${DISPLAY:-}" ] && [ "$(id -u)" = 0 ] && [ "$(tty)" = /dev/tty1 ]; then startx; fi
PROFILE
    if [ "${L2_DESKTOP_AUTOSTART:-1}" = 1 ]; then touch "$ROOTFS/etc/l2/desktop-autostart"; fi
    printf 'Desktop: log in as root on tty1 to start X11 (l2wm). Super+Return opens xterm.\n' >> "$ROOTFS/etc/motd"
}

desktop_rootfs() {
    local ROOTFS=$BUILD/desktop/rootfs target=$DESKTOP_BR/target bin
    local programs=(Xorg xterm startx xinit xauth mcookie xkbcomp l2wm)
    for bin in "${programs[@]}"; do
        [ -x "$target/usr/bin/$bin" ] || die "missing compiled $bin; run: l2 build desktop"
    done
    local path
    for path in lib/ld-musl-x86_64.so.1 usr/lib/xorg/modules/drivers/fbdev_drv.so \
                usr/lib/xorg/modules/input/evdev_drv.so usr/share/X11/xkb/rules/base \
                usr/share/fonts/X11/misc/fonts.dir; do
        [ -e "$target/$path" ] || die "missing desktop runtime file $path; run: l2 build desktop"
    done
    build_rootfs
    for bin in "${programs[@]}"; do
        install -m 0755 "$target/usr/bin/$bin" "$ROOTFS/bin/$bin"
    done
    mkdir -p "$ROOTFS/lib" "$ROOTFS/usr/lib/xorg/modules/drivers" \
             "$ROOTFS/usr/lib/xorg/modules/input" "$ROOTFS/usr/share/X11" \
             "$ROOTFS/usr/share/fonts/X11/misc" "$ROOTFS/usr/share/terminfo"
    # Copy only the Xorg modules actually used (fbdev + evdev).
    cp -a "$target/usr/lib/xorg/modules/drivers/fbdev_drv.so" "$ROOTFS/usr/lib/xorg/modules/drivers/"
    cp -a "$target/usr/lib/xorg/modules/input/evdev_drv.so" "$ROOTFS/usr/lib/xorg/modules/input/"
    for mod in libshadow.so libfbdevhw.so; do
        [ -e "$target/usr/lib/xorg/modules/$mod" ] && cp -a "$target/usr/lib/xorg/modules/$mod" "$ROOTFS/usr/lib/xorg/modules/"
    done
    [ -e "$target/usr/lib/xorg/protocol.txt" ] && cp -a "$target/usr/lib/xorg/protocol.txt" "$ROOTFS/usr/lib/xorg/"
    [ -d "$target/usr/lib/X11" ] && cp -a "$target/usr/lib/X11" "$ROOTFS/usr/lib/"
    # Copy only the transitive NEEDED shared libraries (drops unused libstdc++, libgomp, etc.).
    python3 - "$target" "$ROOTFS" <<'PY'
import os, shutil, subprocess, sys
target, rootfs = sys.argv[1], sys.argv[2]
seeds = [
    os.path.join(rootfs, "bin", b)
    for b in ("Xorg", "xterm", "xinit", "xauth", "mcookie", "xkbcomp", "l2wm")
] + [
    os.path.join(rootfs, "usr/lib/xorg/modules/drivers/fbdev_drv.so"),
    os.path.join(rootfs, "usr/lib/xorg/modules/input/evdev_drv.so"),
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
    # Trimmed X11 data: keep XKB keymaps/rules and pc geometry, drop bulky XML/LST metadata and other geometries.
    cp -a "$target/usr/share/X11/." "$ROOTFS/usr/share/X11/"
    find "$ROOTFS/usr/share/X11/xkb/geometry" -mindepth 1 ! -name pc -delete 2>/dev/null || true
    rm -rf "$ROOTFS/usr/share/X11/xkb/rules/"*.xml \
           "$ROOTFS/usr/share/X11/xkb/rules/"*.lst
    # Minimal X11 bitmap fonts (fixed + cursor + common sizes) instead of 6 MiB of misc fonts.
    local f
    for f in cursor.pcf.gz 6x13-ISO8859-1.pcf.gz 6x13B-ISO8859-1.pcf.gz 6x13.pcf.gz \
             7x13-ISO8859-1.pcf.gz 8x13-ISO8859-1.pcf.gz 9x15-ISO8859-1.pcf.gz 10x20-ISO8859-1.pcf.gz; do
        [ -f "$target/usr/share/fonts/X11/misc/$f" ] && \
            install -m 0644 "$target/usr/share/fonts/X11/misc/$f" "$ROOTFS/usr/share/fonts/X11/misc/$f"
    done
    [ -f "$target/usr/share/fonts/X11/misc/fonts.alias" ] && \
        install -m 0644 "$target/usr/share/fonts/X11/misc/fonts.alias" "$ROOTFS/usr/share/fonts/X11/misc/fonts.alias"
    awk 'NR == 1 { next } { f = $1; if (system("test -f \"'"$ROOTFS"'/usr/share/fonts/X11/misc/" f "\"") == 0) lines[++n] = $0 }
         END { print n + 0; for (i = 1; i <= n; i++) print lines[i] }' \
        "$target/usr/share/fonts/X11/misc/fonts.dir" > "$ROOTFS/usr/share/fonts/X11/misc/fonts.dir"
    # Minimal terminfo set for xterm and Linux console.
    for f in a/ansi d/dumb l/linux v/vt100 v/vt220 x/xterm x/xterm-color x/xterm-256color; do
        if [ -e "$target/usr/share/terminfo/$f" ]; then
            mkdir -p "$ROOTFS/usr/share/terminfo/$(dirname "$f")"
            cp -a "$target/usr/share/terminfo/$f" "$ROOTFS/usr/share/terminfo/$f"
        fi
    done
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
