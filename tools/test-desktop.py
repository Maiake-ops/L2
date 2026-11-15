#!/usr/bin/env python3
"""Boot l2-desktop.iso and test X11, keyboard input and pointer movement.

Uses QEMU's emulated PS/2 devices and serial console; no host X server, root,
network access or KVM is required. Saves a screenshot and guest logs.
"""
import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time
import re

ROOT = Path(__file__).resolve().parent.parent


class Guest:
    def __init__(self, iso, tmp, memory=256):
        serial = str(tmp / 'serial')
        for suffix in ('.in', '.out'):
            os.mkfifo(serial + suffix)
        self.input = os.open(serial + '.in', os.O_RDWR | os.O_NONBLOCK)
        self.output = os.open(serial + '.out', os.O_RDWR | os.O_NONBLOCK)
        self.buffer = b''
        self.qbuffer = b''
        self.process = subprocess.Popen([
            'qemu-system-x86_64', '-m', str(memory), '-smp', '1', '-accel', 'tcg',
            '-vga', 'std', '-display', 'none', '-nic', 'none', '-no-reboot',
            '-chardev', f'pipe,id=serial,path={serial}', '-serial', 'chardev:serial',
            '-qmp', 'stdio', '-monitor', 'none', '-cdrom', str(iso), '-boot', 'd',
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.qread()
        self.qmp('qmp_capabilities')

    def qread(self):
        end = time.monotonic() + 20
        while time.monotonic() < end:
            if b'\n' in self.qbuffer:
                line, self.qbuffer = self.qbuffer.split(b'\n', 1)
                return json.loads(line)
            if select.select([self.process.stdout], [], [], 0.1)[0]:
                chunk = os.read(self.process.stdout.fileno(), 65536)
                if not chunk:
                    raise RuntimeError('QEMU exited: ' + self.process.stderr.read().decode())
                self.qbuffer += chunk
        raise RuntimeError('QMP timed out')

    def qmp(self, command, **arguments):
        self.process.stdin.write((json.dumps({'execute': command, 'arguments': arguments}) + '\n').encode())
        self.process.stdin.flush()
        while True:
            response = self.qread()
            if 'error' in response:
                raise RuntimeError(str(response['error']))
            if 'return' in response:
                return response['return']

    def wait(self, token, timeout=60):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if token in self.buffer:
                return True
            if select.select([self.output], [], [], 0.2)[0]:
                self.buffer += os.read(self.output, 65536)
            if self.process.poll() is not None:
                break
        return token in self.buffer

    def command(self, command, marker, timeout=60):
        self.buffer = b''
        os.write(self.input, command.encode() + b'\n')
        if not self.wait(marker.encode(), timeout):
            raise RuntimeError(f'guest command timed out: {command}\n{self.buffer.decode(errors="replace")}')
        return self.buffer.decode(errors='replace')

    def key(self, key):
        result = self.qmp('human-monitor-command', **{'command-line': 'sendkey ' + key + ' 30'})
        if result:
            raise RuntimeError(result)
        time.sleep(0.08)

    def type(self, text):
        mapping = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus',
                   '_': 'shift-minus', '>': 'shift-dot', '=': 'equal'}
        for char in text:
            self.key(mapping.get(char, 'shift-' + char.lower() if char.isupper() else char))
        self.key('ret')

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        os.close(self.input)
        os.close(self.output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=ROOT / 'build/out/l2-desktop.iso')
    parser.add_argument('--check-install', action='store_true', help='also verify live-account preservation with l2install dir inside the guest')
    args = parser.parse_args()
    logs = ROOT / 'build/logs'
    logs.mkdir(exist_ok=True, parents=True)
    with tempfile.TemporaryDirectory(prefix='l2-desktop-test-') as tmp:
        # The directory-install test keeps both an extracted source rootfs and
        # a destination in the live tmpfs, whose default limit is half of RAM.
        guest = Guest(args.iso, Path(tmp), memory=512 if args.check_install else 256)
        logged_in = False
        try:
            if not guest.wait(b'login:', 90):
                raise RuntimeError('no serial login: ' + guest.buffer.decode(errors='replace'))
            # Exercise first-boot setup through the emulated physical keyboard.
            time.sleep(2)
            for line in ('tester', 'Testpass42', 'Testpass42', 'Testpass42', 'Testpass42'):
                guest.type(line)
                time.sleep(1)
            guest.buffer = b''
            os.write(guest.input, b'root\n')
            if not guest.wait(b'Password:', 10):
                raise RuntimeError('serial password prompt missing')
            guest.buffer = b''
            os.write(guest.input, b'wrongpass\n')
            if not guest.wait(b'Login incorrect', 10):
                raise RuntimeError('incorrect password was not rejected')
            if not guest.wait(b'login:', 10):
                raise RuntimeError('no login retry')
            guest.buffer = b''
            os.write(guest.input, b'root\n')
            if not guest.wait(b'Password:', 10):
                raise RuntimeError('no password retry')
            guest.buffer = b''
            os.write(guest.input, b'Testpass42\n')
            if not guest.wait(b'L2#', 10):
                raise RuntimeError('administrator login failed: ' + guest.buffer.decode(errors='replace'))
            logged_in = True
            print('PASS: account setup, hidden password entry, and rejection of an incorrect password')
            guest.type('Testpass42')
            output = guest.command(
                'n=0; while [ ! -s /run/l2-desktop/display ] && [ "$n" -lt 45 ]; do sleep 1; n=$((n+1)); done; '
                'cat /run/l2-desktop/server.log; '
                'cat /run/l2-desktop/display; echo DESKTOP_\'\'CHECK_DONE', 'DESKTOP_CHECK_DONE')
            print(output)
            if 'no screens found' in output or '\r\n0\r\n' not in output:
                raise RuntimeError('Xorg did not initialize a screen')
            output = guest.command(
                'n=0; while [ ! -f /run/l2-desktop/user/session-ready ] && [ "$n" -lt 15 ]; do sleep 1; n=$((n+1)); done; '
                'sleep 2; '
                'DISPLAY=:0 XAUTHORITY=/run/l2-desktop/Xauthority l2-xprobe; echo DESKTOP_\'\'PROBE_DONE',
                'DESKTOP_PROBE_DONE')
            if 'terminals=1' not in output:
                raise RuntimeError('desktop login did not open xterm: ' + output)
            guest.qmp('screendump', filename=str(logs / 'desktop.ppm'))
            guest.type('id -u >/tmp/gui-input')
            time.sleep(1)
            output = guest.command('cat /tmp/gui-input; echo DESKTOP_\'\'INPUT_DONE', 'DESKTOP_INPUT_DONE')
            if '\r\n1000\r\n' not in output:
                raise RuntimeError('keyboard input did not reach xterm')
            print('PASS: desktop password login opens xterm as uid 1000 and keyboard input reaches its shell')
            guest.key('meta_l-ret')
            time.sleep(2)
            output = guest.command(
                'DISPLAY=:0 XAUTHORITY=/run/l2-desktop/Xauthority l2-xprobe; echo DESKTOP_\'\'PS_DONE', 'DESKTOP_PS_DONE')
            if 'terminals=2' not in output:
                raise RuntimeError('Super+Return did not open another xterm: ' + output)
            print('PASS: Super+Return opens a second xterm')
            guest.qmp('input-send-event', events=[
                {'type': 'rel', 'data': {'axis': 'x', 'value': -400}},
            ])
            time.sleep(1)
            output = guest.command(
                'DISPLAY=:0 XAUTHORITY=/run/l2-desktop/Xauthority l2-xprobe; echo DESKTOP_\'\'BEFORE_DONE', 'DESKTOP_BEFORE_DONE')
            before = re.search(r'pointer=(\d+),(\d+) focus=(\d+)', output).groups()
            guest.qmp('input-send-event', events=[
                {'type': 'rel', 'data': {'axis': 'x', 'value': 1000}},
                {'type': 'rel', 'data': {'axis': 'y', 'value': 50}},
            ])
            time.sleep(1)
            output = guest.command(
                'DISPLAY=:0 XAUTHORITY=/run/l2-desktop/Xauthority l2-xprobe; echo DESKTOP_\'\'MOUSE_DONE', 'DESKTOP_MOUSE_DONE')
            after = re.search(r'pointer=(\d+),(\d+) focus=(\d+)', output).groups()
            if before[:2] == after[:2]:
                raise RuntimeError('mouse pointer did not move in X11')
            if before[2] == after[2]:
                raise RuntimeError(f'entering the other terminal did not change keyboard focus: {before} -> {after}')
            guest.qmp('input-send-event', events=[
                {'type': 'btn', 'data': {'button': 'left', 'down': True}},
                {'type': 'btn', 'data': {'button': 'left', 'down': False}},
            ])
            guest.qmp('screendump', filename=str(logs / 'desktop.ppm'))
            print('PASS: mouse movement changes the pointer and terminal focus; left click delivered')
            guest.key('meta_l-esc')
            output = guest.command(
                'n=0; while pidof l2wm >/dev/null && [ "$n" -lt 10 ]; do sleep 1; n=$((n+1)); done; '
                'sleep 2; pidof l2wm || echo WM_\'\'STOPPED; cat /sys/class/tty/tty0/active; echo DESKTOP_\'\'LOGOUT_DONE',
                'DESKTOP_LOGOUT_DONE')
            if 'WM_STOPPED' not in output or 'tty1' not in output:
                raise RuntimeError('logout did not return to the login console: ' + output)
            print('PASS: Super+Escape ends the desktop and returns to the login console')
            output = guest.command(
                'n=0; while ! pidof login >/dev/null && [ "$n" -lt 15 ]; do sleep 1; n=$((n+1)); done; '
                'if pidof login >/dev/null; then echo LOGIN_\'\'AVAILABLE; else ps; fi; '
                'echo DESKTOP_\'\'LOGIN_READY', 'DESKTOP_LOGIN_READY')
            if 'LOGIN_AVAILABLE' not in output:
                raise RuntimeError('desktop did not present a new password login: ' + output)
            guest.type('Testpass42')
            output = guest.command(
                'n=0; while [ "$n" -lt 30 ]; do '
                'if DISPLAY=:0 XAUTHORITY=/run/l2-desktop/Xauthority l2-xprobe 2>/dev/null | grep -q "^terminals=1$"; '
                'then echo RELOGIN_\'\'OK; break; fi; sleep 1; n=$((n+1)); done; echo DESKTOP_\'\'RELOGIN_DONE',
                'DESKTOP_RELOGIN_DONE')
            if 'RELOGIN_OK' not in output:
                raise RuntimeError('logging in again did not restart the desktop: ' + output)
            print('PASS: password login starts a fresh desktop after logout')
            if args.check_install:
                output = guest.command(
                    'l2install -y --no-wifi --no-ssl dir /tmp/l2-installed && '
                    'cmp /etc/shadow /tmp/l2-installed/etc/shadow && '
                    'cmp /etc/passwd /tmp/l2-installed/etc/passwd && '
                    "su - tester -c 'test -w /tmp/l2-installed/home/tester' && "
                    'test -u /tmp/l2-installed/bin/busybox && '
                    'echo ACCOUNT_\'\'PRESERVED; echo DESKTOP_\'\'INSTALL_DONE',
                    'DESKTOP_INSTALL_DONE', timeout=180)
                if 'ACCOUNT_PRESERVED' not in output:
                    raise RuntimeError('installation did not preserve the account and ownership: ' + output)
                print('PASS: installation preserves password hashes, user IDs, home ownership and passwd/su permissions')
        finally:
            try:
                guest.qmp('screendump', filename=str(logs / 'desktop-last.ppm'))
                output = guest.buffer.decode(errors='replace')
                if logged_in:
                    output = guest.command('cat /run/l2-desktop/*.log /run/l2-desktop/user/*.log; echo DESKTOP_\'\'LOG_DONE', 'DESKTOP_LOG_DONE')
                (logs / 'desktop-guest.log').write_text(output)
            finally:
                guest.close()


if __name__ == '__main__':
    main()
