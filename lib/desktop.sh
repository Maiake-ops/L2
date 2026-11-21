# Optional Buildroot packages. L2 still owns init, the kernel, and ISO assembly.
DESKTOP_BR=$BUILD/desktop/buildroot

desktop_packages() (
    mkdir -p "$BUILD/desktop"
    exec 9>"$BUILD/desktop/build.lock"
    flock 9
    local version archive dir make_bin=${L2_DESKTOP_MAKE:-make}
    version=$(lock_get buildroot_version)
    archive=buildroot-$version.tar.xz
    dir=$SRC/buildroot-$version
    local sha; sha=$(lock_get buildroot_sha256)
    [[ $sha =~ ^[0-9a-f]{64}$ ]] || die "missing Buildroot checksum in sources.lock"
    download "https://buildroot.org/downloads/$archive" "$CACHE/$archive" "$sha" "Buildroot $version"
    if [ ! -f "$dir/Makefile" ]; then tar xf "$CACHE/$archive" -C "$SRC"; fi
    if [ "$DISTRO" = nixos ] && [ -z "${L2_DESKTOP_MAKE:-}" ]; then
        local fhs; fhs=$(nix-build "$L2_ROOT/desktop/fhs.nix" --no-out-link)
        make_bin=$fhs/bin/l2-desktop-make
    fi
    local args=(-C "$dir" "O=$DESKTOP_BR" "BR2_EXTERNAL=$L2_ROOT/desktop"
                "BR2_DL_DIR=$CACHE/buildroot" "BR2_DEFCONFIG=$L2_ROOT/desktop/buildroot.config"
                "BR2_JLEVEL=$JOBS")
    local log=$LOGS/desktop-packages.log
    step "Building desktop package(s): $* (log: $log)"
    "$make_bin" "${args[@]}" defconfig > "$log" 2>&1 || { ui_logtail "$log"; die "Buildroot configuration failed"; }
    "$make_bin" "${args[@]}" -j"$JOBS" -l"$LOAD" "$@" >> "$log" 2>&1 ||
        { ui_logtail "$log"; die "desktop build failed (log: $log)"; }
)

build_desktop_config() {
    cp -a "$L2_ROOT/desktop/rootfs/." "$ROOTFS/"
    mkdir -p "$ROOTFS"/{var/log,var/lib/xkb,root}
    chmod 1777 "$ROOTFS/tmp"
    # A passwordless local root login matches L2's live console access.
    sed -i 's/^root:x:/root::/' "$ROOTFS/etc/passwd"
    sed -i 's|^service tty1 .*|service tty1 respawn /bin/getty 0 tty1 linux|' "$ROOTFS/etc/rcl2.conf"
    cat >> "$ROOTFS/etc/profile" <<'PROFILE'
if [ -f /etc/l2/desktop-autostart ] && [ "$(tty)" = /dev/tty1 ]; then startx; fi
PROFILE
    if [ "${L2_DESKTOP_AUTOSTART:-0}" = 1 ]; then touch "$ROOTFS/etc/l2/desktop-autostart"; fi
    printf 'Desktop: log in as root on tty1 and run startx. Super+Return opens xterm.\n' >> "$ROOTFS/etc/motd"
}

desktop_rootfs() {
    local ROOTFS=$BUILD/desktop/rootfs target=$DESKTOP_BR/target bin
    for bin in Xorg xterm xinit xauth xkbcomp l2wm; do
        [ -x "$target/usr/bin/$bin" ] || die "missing compiled $bin; run: l2 build desktop"
    done
    local path
    for path in lib/ld-musl-x86_64.so.1 usr/lib/xorg/modules/drivers/fbdev_drv.so \
                usr/lib/xorg/modules/input/evdev_drv.so usr/share/X11/xkb/rules/base \
                usr/share/fonts/X11/misc/fonts.dir; do
        [ -e "$target/$path" ] || die "missing desktop runtime file $path; run: l2 build desktop"
    done
    build_rootfs
    for bin in Xorg xterm xinit xauth xkbcomp l2wm; do
        install -m 0755 "$target/usr/bin/$bin" "$ROOTFS/bin/$bin"
    done
    mkdir -p "$ROOTFS/usr/lib" "$ROOTFS/usr/share"
    cp -a "$target/lib/"*.so* "$ROOTFS/lib/"
    cp -a "$target/usr/lib/"*.so* "$target/usr/lib/xorg" "$ROOTFS/usr/lib/"
    cp -a "$target/usr/share/"{X11,fonts,terminfo} "$ROOTFS/usr/share/"
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
    local c
    build_musl
    [ -s "$OUT/vmlinuz-l2" ] || build_kernel
    for c in busybox dash rcl2 l2pm l2wifi; do [ -x "$OUT/$c" ] || build_one "$c"; done
    [ -x "$OUT/limine/bin/limine" ] || build_limine
    [ -d "$BUILD/hw" ] || build_hwpack
    if [ ! -x "$BUILD/installtools/bin/limine" ]; then
        [ -x "$OUT/l2gpt" ] || build_l2gpt
        [ -x "$OUT/mke2fs" ] || build_e2fsprogs
        [ -x "$OUT/mkfs.fat" ] || build_dosfstools
        build_installpack
    fi
    desktop_packages all
    build_iso desktop
}
