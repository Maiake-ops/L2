#!/usr/bin/env python3
"""Stage the cached Nix Xorg/fbdev/evdev/xterm runtime, without a Nix daemon.

Only runtime ELF dependencies and required keyboard/font data are copied.
Absolute store paths are preserved so binaries need no patching. The resulting
rootfs runs independently of the build host and does not need Nix installed.
"""
import argparse
import hashlib
import re
import shlex
import shutil
import subprocess
from pathlib import Path


def package(name, relative):
    candidates = [p for p in Path('/nix/store').glob(f'*-{name}-*') if (p / relative).is_file()]
    if not candidates:
        raise RuntimeError(f'missing cached Nix package {name} ({relative})')
    return sorted(candidates, key=lambda p: (tuple(map(int, re.findall(r'\d+', p.name[33:]))), p.name))[-1]


class Runtime:
    def __init__(self, root):
        self.root = root
        self.files = set()

    def copy(self, source, destination=None):
        source = Path(source)
        destination = self.root / str(destination or source).lstrip('/')
        if destination in self.files:
            return
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination, follow_symlinks=True)
        self.files.add(destination)

    def elf(self, source, loader=None):
        source = Path(source)
        if source.read_bytes()[:4] != b'\x7fELF':
            raise RuntimeError(f'expected an ELF binary: {source}')
        headers = subprocess.check_output(['readelf', '-l', str(source)], text=True)
        interpreter = re.search(r'Requesting program interpreter: (.*?)\]', headers)
        loader = interpreter[1] if interpreter else loader
        if not loader:
            raise RuntimeError(f'no dynamic loader for {source}')
        result = subprocess.run([loader, '--list', str(source)], text=True, capture_output=True)
        if result.returncode or 'not found' in result.stdout:
            raise RuntimeError(f'unresolved dependencies for {source}: {result.stdout}{result.stderr}')
        self.copy(source)
        self.copy(loader)
        for path in re.findall(r'(?:=>\s+|^\s*)(/[^\s]+)', result.stdout, re.M):
            self.copy(path)
        return loader

    def tree(self, source):
        for path in Path(source).rglob('*'):
            if path.is_file():
                self.copy(path)


def stage(root):
    runtime = Runtime(root)
    xorg = package('xorg-server', 'bin/Xorg')
    xterm = package('xterm', 'bin/.xterm-wrapped')
    xauth = package('xauth', 'bin/xauth')
    fbdev = package('xf86-video-fbdev', 'lib/xorg/modules/drivers/fbdev_drv.so')
    evdev = package('xf86-input-evdev', 'lib/xorg/modules/input/evdev_drv.so')
    misc = package('font-misc-misc', 'share/fonts/X11/misc/6x13.pcf.gz')
    cursor = package('font-cursor-misc', 'share/fonts/X11/misc/cursor.pcf.gz')
    server = xorg / 'bin/Xorg'
    loader = runtime.elf(server)
    runtime.elf(xterm / 'bin/.xterm-wrapped')
    runtime.elf(xauth / 'bin/xauth')
    for module in (xorg / 'lib/xorg/modules/libfbdevhw.so',
                   xorg / 'lib/xorg/modules/libshadow.so',
                   fbdev / 'lib/xorg/modules/drivers/fbdev_drv.so',
                   evdev / 'lib/xorg/modules/input/evdev_drv.so'):
        runtime.elf(module, loader)
    # Xorg invokes this compiled-in xkbcomp path to build keyboard maps.
    data = server.read_bytes()
    xkb_bin = re.search(rb'/nix/store/[^/\x00\s]+-xkbcomp-[^/\x00\s]+/bin', data)
    xkb_data = re.search(rb'/nix/store/[^/\x00\s]+-xkeyboard-config-[^/\x00\s]+/share/X11/xkb', data)
    if not xkb_bin or not xkb_data:
        raise RuntimeError('cannot locate the Xorg keyboard runtime')
    runtime.elf(Path(xkb_bin[0].decode()) / 'xkbcomp')
    runtime.tree(xkb_data[0].decode())
    runtime.tree(xterm / 'lib/X11/app-defaults')
    terminfo = package('ncurses', 'share/terminfo/x/xterm') / 'share/terminfo'
    for name in ('xterm', 'xterm-256color', 'linux', 'vt100'):
        runtime.copy(terminfo / name[0] / name, '/opt/l2-desktop/terminfo/' + name[0] + '/' + name)
    font_entries = []
    for pkg, names in ((misc, {'6x13.pcf.gz', '9x15.pcf.gz'}), (cursor, {'cursor.pcf.gz'})):
        directory = pkg / 'share/fonts/X11/misc'
        for entry in (directory / 'fonts.dir').read_text().splitlines()[1:]:
            name = entry.split()[0]
            if name in names:
                runtime.copy(directory / name, '/opt/l2-desktop/fonts/' + name)
                font_entries.append(entry)
    fonts = root / 'opt/l2-desktop/fonts'
    (fonts / 'fonts.dir').write_text(str(len(font_entries)) + '\n' + '\n'.join(font_entries) + '\n')
    fixed = next(line.split(maxsplit=1)[1] for line in font_entries if line.startswith('6x13.pcf.gz '))
    (fonts / 'fonts.alias').write_text(f'fixed {fixed}\n')
    config = root / 'etc/l2/desktop-runtime.sh'
    config.parent.mkdir(parents=True, exist_ok=True)
    values = {
        'L2_XORG': str(server),
        'L2_XMODULES': ','.join(str(p / 'lib/xorg/modules') for p in (fbdev, evdev, xorg)),
        'L2_XTERM': str(xterm / 'bin/.xterm-wrapped'),
        'L2_XAUTH': str(xauth / 'bin/xauth'),
        'XAPPLRESDIR': str(xterm / 'lib/X11/app-defaults'),
    }
    config.write_text(''.join(f'{key}={shlex.quote(value)}\n' for key, value in values.items()))
    manifest = root / 'etc/l2/desktop-runtime.sha256'
    manifest.write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  /{p.relative_to(root)}\n'
                                for p in sorted(runtime.files)))
    size = sum(p.stat().st_size for p in runtime.files)
    print(f'desktop runtime: {len(runtime.files)} files, {size // 1024 // 1024} MiB; {xorg.name[33:]}, {xterm.name[33:]}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rootfs', type=Path)
    args = parser.parse_args()
    try:
        stage(args.rootfs.resolve())
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'desktop-runtime: {error}\n')


if __name__ == '__main__':
    main()
