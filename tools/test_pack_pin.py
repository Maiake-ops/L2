"""Phase-2 co-signed pack integrity pinning & udhcpc hardening tests (M17).

Covers docs/armageddon/p2-pack-pin-contract.md (A1/A2/A3):
- l2net verify_pack & use_pack fail-closed enforcement
- overlay/bin/openssl Phase-2 ssl pack verification before exec
- l2audio.c compile & Phase-2 wifi pack verification before symlink
- overlay/etc/l2/udhcpc.script input validation & glob/injection defense
"""

import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parent.parent
L2NET = REPO / "overlay" / "bin" / "l2net"
OPENSSL_WRAPPER = REPO / "overlay" / "bin" / "openssl"
UDHCPC = REPO / "overlay" / "etc" / "l2" / "udhcpc.script"
L2AUDIO_C = REPO / "l2audio" / "l2audio.c"
DASH = REPO / "build" / "out" / "dash"
MUSL_CC = REPO / "build" / "bin" / "musl-gcc"


def write_manifest(pack_dir: Path) -> str:
    lines = []
    for p in sorted(pack_dir.rglob("*")):
        if p.is_file() and not p.is_symlink() and p.name != "pack-manifest":
            rel = p.relative_to(pack_dir).as_posix()
            h = hashlib.sha256(p.read_bytes()).hexdigest()
            lines.append(f"{h}  {rel}\n")
    mf = pack_dir / "pack-manifest"
    mf.write_text("".join(lines))
    return hashlib.sha256(mf.read_bytes()).hexdigest()


def write_pin(pin_path: Path, entries: dict[str, str]):
    pin_path.write_text("".join(f"{entries[k]}  {k}\n" for k in sorted(entries)))


@unittest.skipUnless(DASH.exists(), "shipped dash (build/out/dash) missing")
class TestPackPin(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="pack-pin-")
        self.addCleanup(self.tmp.cleanup)
        self.scratch = Path(self.tmp.name)
        self.krel = Path("/proc/sys/kernel/osrelease").read_text().strip()
        self.media = self.scratch / "media"
        self.mods = self.scratch / "modules"
        self.fwpath = self.scratch / "fwpath"
        self.fwpath.touch()
        self.pinfile = self.scratch / "pack-hash"
        self.vercache = self.scratch / "vercache"
        self.env = dict(
            os.environ,
            L2_MEDIUM=str(self.media),
            L2_MEDIA=str(self.media),
            L2_MODDIR=str(self.mods),
            L2_FW_PATH=str(self.fwpath),
            L2_PACK_HASH=str(self.pinfile),
            L2_PACK_CACHE=str(self.vercache),
            L2_L2NET=str(L2NET),
        )

    def _make_pack(self, kind: str, with_marker: bool = True, with_pin: bool = True):
        pack = self.media / "l2" / kind
        moddir = pack / "lib" / "modules" / self.krel
        moddir.mkdir(parents=True, exist_ok=True)
        (moddir / "drv.ko").write_bytes(b"fake-ko")
        (pack / "lib" / "firmware").mkdir(parents=True, exist_ok=True)
        (pack / "lib" / "firmware" / "fw.bin").write_bytes(b"fake-fw")
        if with_marker:
            (self.media / "l2" / "L2-LIVE").write_text("L2 live\n")
        h = write_manifest(pack)
        if with_pin:
            write_pin(self.pinfile, {kind: h})
        return pack

    def _run_l2net(self, *args, env=None):
        return subprocess.run(
            [str(DASH), str(L2NET), *args],
            env=env or self.env,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def test_syntax_checks(self):
        for script in (L2NET, OPENSSL_WRAPPER, UDHCPC):
            r = subprocess.run(["sh", "-n", str(script)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f"{script}: {r.stderr}")

    def test_genuine_pinned_pack_activates_and_caches(self):
        pack = self._make_pack("hw")
        r = self._run_l2net("verify-pack", str(pack), "hw")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue((self.vercache / "hw").exists())

    def test_missing_or_empty_pin_refused(self):
        pack = self._make_pack("hw", with_pin=False)
        r = self._run_l2net("verify-pack", str(pack), "hw")
        self.assertNotEqual(r.returncode, 0)
        self.pinfile.write_text("")
        r2 = self._run_l2net("verify-pack", str(pack), "hw")
        self.assertNotEqual(r2.returncode, 0)

    def test_tampered_firmware_refused_before_activation(self):
        pack = self._make_pack("wifi")
        (pack / "lib" / "firmware" / "fw.bin").write_bytes(b"TAMPERED-FW")
        r = self._run_l2net("verify-pack", str(pack), "wifi")
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse((self.mods / self.krel).exists())
        self.assertEqual(self.fwpath.read_text(), "")

    def test_extra_unmanifested_file_refused(self):
        pack = self._make_pack("hw")
        (pack / "evil.ko").write_bytes(b"smuggled")
        r = self._run_l2net("verify-pack", str(pack), "hw")
        self.assertNotEqual(r.returncode, 0)

    def test_a2_malformed_manifests_and_symlinks_refused(self):
        pack = self._make_pack("hw")
        # 1. Symlink in pack tree
        (pack / "link.ko").symlink_to("/etc/passwd")
        self.assertNotEqual(self._run_l2net("verify-pack", str(pack), "hw").returncode, 0)
        (pack / "link.ko").unlink()

        # 2. Malformed manifest variants (even if pinned to match manifest sha256!)
        bad_manifests = [
            f"{'a'*64}  ../escape\n",
            f"{'a'*64}  /etc/passwd\n",
            f"{'a'*64}  lib/modules/{self.krel}/drv.ko\r\n",
            f"{'A'*64}  lib/modules/{self.krel}/drv.ko\n",  # uppercase hex
            f"{'a'*63}  lib/modules/{self.krel}/drv.ko\n",  # short hash
            (
                f"{hashlib.sha256(b'fake-ko').hexdigest()}  lib/modules/{self.krel}/drv.ko\n"
                f"{hashlib.sha256(b'fake-ko').hexdigest()}  lib/modules/{self.krel}/drv.ko\n"
            ),  # duplicate relpath
        ]
        for content in bad_manifests:
            mf = pack / "pack-manifest"
            mf.write_bytes(content.encode())
            write_pin(self.pinfile, {"hw": hashlib.sha256(mf.read_bytes()).hexdigest()})
            if (self.vercache / "hw").exists():
                (self.vercache / "hw").unlink()
            r = self._run_l2net("verify-pack", str(pack), "hw")
            self.assertNotEqual(r.returncode, 0, f"accepted bad manifest: {content!r}")

    def test_openssl_wrapper_verifies_ssl_pack(self):
        ssl = self.media / "l2" / "ssl"
        (ssl / "bin").mkdir(parents=True)
        fake_ossl = ssl / "bin" / "openssl"
        fake_ossl.write_text("#!/bin/sh\necho VERIFIED_OPENSSL_OK\n")
        fake_ossl.chmod(0o755)
        (self.media / "l2" / "L2-LIVE").write_text("L2 live\n")
        # Without pin -> refused
        r_bad = subprocess.run(
            [str(DASH), str(OPENSSL_WRAPPER), "version"],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertNotEqual(r_bad.returncode, 0)
        self.assertNotIn("VERIFIED_OPENSSL_OK", r_bad.stdout)
        # With valid pin -> runs
        h = write_manifest(ssl)
        write_pin(self.pinfile, {"ssl": h})
        r_ok = subprocess.run(
            [str(DASH), str(OPENSSL_WRAPPER), "version"],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(r_ok.returncode, 0, r_ok.stdout + r_ok.stderr)
        self.assertIn("VERIFIED_OPENSSL_OK", r_ok.stdout)
        # Tamper binary -> refused (after clearing cache)
        if (self.vercache / "ssl").exists():
            (self.vercache / "ssl").unlink()
        fake_ossl.write_text("#!/bin/sh\necho PWNED\n")
        r_tamper = subprocess.run(
            [str(DASH), str(OPENSSL_WRAPPER), "version"],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertNotEqual(r_tamper.returncode, 0)
        self.assertNotIn("PWNED", r_tamper.stdout)

    def test_l2audio_compiles_and_enforces_pack_pin(self):
        cc = str(MUSL_CC) if MUSL_CC.exists() else "cc"
        out_bin = self.scratch / "l2audio"
        cmd = [cc, "-std=c11", "-Os", "-Wall", "-Wextra", "-o", str(out_bin), str(L2AUDIO_C), "-lm"]
        if MUSL_CC.exists():
            cmd.insert(3, "-static")
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        # Run `l2audio status` cleanly
        r_st = subprocess.run([str(out_bin), "status"], capture_output=True, text=True, timeout=15)
        self.assertEqual(r_st.returncode, 0, r_st.stderr)

    def test_udhcpc_script_hardening(self):
        shimdir = self.scratch / "shimbin"
        shimdir.mkdir()
        ip_log = self.scratch / "ip.log"
        ip_shim = shimdir / "ip"
        ip_shim.write_text(f'#!/bin/sh\nprintf "%s\\n" "$*" >> "{ip_log}"\n')
        ip_shim.chmod(0o755)
        resolv = self.scratch / "resolv.conf"
        resolv.write_text("nameserver 1.1.1.1\n")

        # 1. Invalid interface rejected
        env_bad_if = dict(
            self.env,
            PATH=f"{shimdir}:{self.env['PATH']}",
            L2_RESOLV_CONF=str(resolv),
            interface="-evil",
            ip="10.0.0.2",
        )
        r = subprocess.run([str(DASH), str(UDHCPC), "bound"], env=env_bad_if, capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)

        # 2. Invalid IP rejected
        env_bad_ip = dict(env_bad_if, interface="eth0", ip="10.0.0.999")
        r = subprocess.run([str(DASH), str(UDHCPC), "bound"], env=env_bad_ip, capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)

        # 3. Malicious domain + glob in router/dns + valid IP
        (self.scratch / "10.0.0.1").write_text("glob-trap")
        env_ok = dict(
            env_bad_if,
            interface="eth0",
            ip="10.0.0.5",
            mask="24",
            broadcast="10.0.0.255",
            router="* 10.0.0.1",
            domain="evil\nnameserver 6.6.6.6",
            dns="8.8.8.8 bad;cmd * 9.9.9.9",
        )
        r = subprocess.run(
            [str(DASH), str(UDHCPC), "bound"],
            env=env_ok,
            cwd=str(self.scratch),
            capture_output=True,
            text=True,
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        res_text = resolv.read_text()
        self.assertNotIn("6.6.6.6", res_text)
        self.assertNotIn("search ", res_text)
        self.assertEqual(res_text, "nameserver 8.8.8.8\nnameserver 9.9.9.9\n")
        ip_lines = ip_log.read_text().splitlines()
        self.assertIn("addr add 10.0.0.5/24 broadcast 10.0.0.255 dev eth0", ip_lines)
        self.assertIn("route replace default via 10.0.0.1 dev eth0", ip_lines)


if __name__ == "__main__":
    unittest.main()
