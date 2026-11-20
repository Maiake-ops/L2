# l2wm

Small X11 tiling window manager. C owns the Xlib event loop; a dependency-free
Rust static library computes a master-and-stack layout. This is an initial WM,
without workspaces, EWMH support, floating dialogs, or multi-monitor layouts.

## Build and run

Install a C compiler, rustc, pkg-config and Xlib development headers/libraries.
Run `make -C l2wm`, then `startx /absolute/path/to/l2wm/build/l2wm` on a machine
with an X server and xinit installed. Do not run alongside another WM.
`make -C l2wm test` tests the Rust layout without X11 dependencies.

Super+Return launches xterm (override the executable with L2WM_TERMINAL).
Super+j/k changes focus (active windows have a `#7ebae4` border), Super+m swaps
the focused window into the master slot, Super+h/l resizes the master split,
Super+f toggles monocle (fullscreen) layout, Super+q closes the focused client,
and Super+Escape exits. Pointer entry also changes focus. Num Lock assumes the
usual Mod2 mapping.

## Optional L2 integration

The top-level `l2` script remains outside this directory. Normal L2 builds do
not build or install l2wm and need no Rust or X11 dependencies.

`./l2 build l2wm` builds a static distro binary into build/out/l2wm.
`L2_WITH_L2WM=1 ./l2 build all` also installs it in the rootfs. To rebuild an
already prepared rootfs, use `L2_WITH_L2WM=1 ./l2 build rootfs initramfs iso`.
Rebuilding without this flag omits l2wm even if an older binary exists.

Distro builds automatically build musl when needed and download/build static
X11 dependencies (xorgproto, xtrans, xcb-proto, libXau, libxcb and libX11).
These are isolated in build/l2wm-x11 and are built only when l2wm is selected.
Source archives use the builder's cache and pin-on-first-download SHA256 checks.
The build host needs rustc, a C toolchain, make, pkg-config, Python 3, readelf,
and curl or wget. Logs are saved as build/logs/l2wm-*.log.
L2WM_CC can override the target compiler. Remove build/l2wm-x11 to rebuild the
X11 dependencies after changing compilers. The build rejects dynamic binaries.

The `l2wm` component installs the WM only. `./l2 build desktop` builds the
complete `build/out/l2-desktop.iso` with Xorg, framebuffer/evdev drivers,
xterm, fonts and keyboard maps. It starts account setup on first boot, then
opens the desktop after password login. The desktop runs as the new user;
use the administrator password with `su -` for root access. Text logins are
available on Ctrl+Alt+F2 and serial. Super+Escape logs out.

This optional image imports cached Nix runtime packages (including their
shared libraries); l2wm itself remains static. No Nix daemon or Nix on the
target is needed. The desktop image has a separate 128 MiB size budget and
requires a firmware framebuffer and at least 256 MiB of RAM. Connect keyboard
and mouse before starting the session. See the main README for runtime
overrides and `python3 tools/test-desktop.py` for the QEMU integration test.
