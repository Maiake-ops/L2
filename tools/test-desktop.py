#!/usr/bin/env python3
"""Exercise tty login, X11 keyboard input, WM layout reload and poweroff from a real ISO."""
import argparse
from pathlib import Path
import socket
import tempfile
import time

from ramtest import Guest, PROMPT, accel, find_ovmf


def command(guest, script):
    guest.buf = b""
    guest.send(script + "; echo L2GUI''DONE")
    if not guest.read_until(b"L2GUIDONE\r", 60):
        raise RuntimeError("serial command timed out: " + repr(guest.buf))
    return guest.buf.decode(errors="replace")


def require(guest, condition):
    result = command(guest, 'n=0; until ' + condition +
                     '; do n=$((n+1)); [ "$n" -lt 20 ] || break; sleep 1; done; ' +
                     condition + " && echo L2GUI''PASS")
    if "\nL2GUIPASS\r" not in result:
        raise RuntimeError("guest check failed: " + result)


def key(monitor, name):
    monitor.sendall(f"sendkey {name} 35\n".encode())
    time.sleep(0.09)


def type_line(monitor, line):
    names = {" ": "spc", "/": "slash", ">": "shift-dot", ".": "dot", "-": "minus"}
    for char in line:
        key(monitor, names.get(char, char))
    key(monitor, "ret")


def test(iso, firmware, temp, autostart):
    monitor_path = str(Path(temp) / "monitor")
    argv = ["qemu-system-x86_64", "-m", "512", "-vga", "std", "-display", "none",
            "-serial", "stdio", "-monitor", f"unix:{monitor_path},server,nowait",
            "-nic", "none", "-no-reboot", "-boot", "d", "-cdrom", str(iso)] + accel()
    if firmware:
        argv += ["-drive", f"if=pflash,format=raw,readonly=on,file={firmware}"]
    guest = Guest(argv)
    try:
        if not guest.read_until(PROMPT, 120):
            raise RuntimeError("no serial shell: " + repr(guest.buf[-2000:]))
        with socket.socket(socket.AF_UNIX) as monitor:
            monitor.connect(monitor_path)
            require(guest, "pidof getty >/dev/null")
            type_line(monitor, "root")
            time.sleep(1)
            if not autostart:
                type_line(monitor, "startx")
            require(guest, "pidof Xorg && pidof l2wm && pidof xterm")
            key(monitor, "meta_l-ret")
            require(guest, 'set -- $(pidof xterm) && [ "$#" = 2 ] && set -- /dev/pts/[0-9]* && [ "$#" -ge 2 ]')
            time.sleep(0.5)
            for side, distance in (("left", -1600), ("right", 1600)):
                monitor.sendall(f"mouse_move {distance} 0\n".encode())
                time.sleep(0.3)
                type_line(monitor, f"tty >/tmp/{side}")
                require(guest, f"test -s /tmp/{side}")
            require(guest, "! cmp -s /tmp/left /tmp/right")
            type_line(monitor, "stty size >/tmp/before")
            require(guest, "test -s /tmp/before")
            command(guest, "echo master_percent=70 > /etc/l2/l2wm.conf")
            key(monitor, "meta_l-r")
            type_line(monitor, "stty size >/tmp/after")
            require(guest, "test -s /tmp/after && ! cmp -s /tmp/before /tmp/after")
            # Exercise interactive master resize, workspace switch, and window close
            key(monitor, "meta_l-h")
            key(monitor, "meta_l-2")
            key(monitor, "meta_l-1")
            key(monitor, "meta_l-q")
            require(guest, 'set -- $(pidof xterm) && [ "$#" = 1 ]')
            key(monitor, "meta_l-esc")
            require(guest, "! pidof Xorg && ! pidof l2wm && ! pidof xterm")
        result = command(guest, "l2install dir /installed && test -x /installed/bin/l2wm && "
                         "test -s /installed/boot/initramfs && echo L2INSTALL''PASS")
        if "\nL2INSTALLPASS\r" not in result:
            raise RuntimeError("desktop directory install failed: " + result)
        guest.send("poweroff")
        if guest.p.wait(timeout=45):
            raise RuntimeError("guest poweroff failed")
        print("PASS: console login, startx, Xorg, l2wm, two xterms, keyboard input, pointer focus, layout reload, logout, directory install and poweroff")
    except Exception:
        if guest.p.poll() is None:
            print(command(guest, "ps; cat /dev/vcs1; tail -n 65 /var/log/Xorg.0.log"))
        raise
    finally:
        guest.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("--uefi", action="store_true")
    parser.add_argument("--autostart", action="store_true", help="expect X to start on tty1 login")
    parser.add_argument("--ovmf", help="override firmware detection")
    args = parser.parse_args()
    firmware = find_ovmf(args.ovmf) if args.uefi else None
    if args.uefi and not firmware:
        parser.error("use --ovmf to specify the OVMF firmware")
    with tempfile.TemporaryDirectory() as directory:
        test(args.iso, firmware, directory, args.autostart)
