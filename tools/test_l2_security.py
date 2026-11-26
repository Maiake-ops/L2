"""Security and supply-chain regression tests for ./l2 and l2fetch (M16)."""

import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parent.parent
L2 = REPO / "l2"
L2FETCH = REPO / "l2fetch"


def extract_l2_functions():
    """Return ./l2 without its trailing `main "$@"` invocation."""
    src = L2.read_text()
    marker = '\nmain "$@"'
    idx = src.rfind(marker)
    assert idx != -1, "could not locate main invocation in ./l2"
    return src[:idx] + "\n"


class TestL2Security(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="l2-sec-")
        self.addCleanup(self.tmp.cleanup)
        self.scratch = Path(self.tmp.name)
        self.l2_lib = self.scratch / "l2_lib.sh"
        self.l2_lib.write_text(extract_l2_functions())
        self.env = dict(
            os.environ,
            L2_ROOT=str(REPO),
            L2_BUILD_DIR=str(self.scratch / "build"),
            L2_PLAIN="1",
            NO_COLOR="1",
            SOURCE_DATE_EPOCH="1767225600",
        )

    def run_l2_snippet(self, snippet, env=None, ok=True):
        script = f'source "{self.l2_lib}"\ninit_epoch\n{snippet}\n'
        r = subprocess.run(
            ["bash", "-Eeuo", "pipefail", "-c", script],
            env=env or self.env,
            cwd=str(self.scratch),
            capture_output=True,
            text=True,
            timeout=30,
        )
        if ok:
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        else:
            self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
        return r

    # -------------------------------------------------------- syntax & static
    def test_syntax_checks(self):
        r1 = subprocess.run(["bash", "-n", str(L2)], capture_output=True, text=True)
        self.assertEqual(r1.returncode, 0, r1.stderr)
        r2 = subprocess.run(["sh", "-n", str(L2FETCH)], capture_output=True, text=True)
        self.assertEqual(r2.returncode, 0, r2.stderr)

    def test_dash_uses_https_only(self):
        src = L2.read_text()
        self.assertNotIn("http://gondor.apana.org.au", src)
        self.assertIn("https://gondor.apana.org.au/~herbert/dash/files/", src)

    def test_no_source_os_release(self):
        self.assertNotIn(". /etc/os-release", L2.read_text())
        self.assertNotIn(". /etc/os-release", L2FETCH.read_text())

    # -------------------------------------------------------- klocal_set
    def test_klocal_set_valid_and_idempotent(self):
        klocal = self.scratch / "kernel.local"
        self.run_l2_snippet(
            f'KLOCAL="{klocal}"\n'
            'klocal_set "CONFIG_FOO=y"\n'
            'klocal_set "CONFIG_NR_CPUS=8"\n'
            'klocal_set "CONFIG_HEX=0x1f"\n'
            'klocal_set \'CONFIG_STR="-l2"\'\n'
            'klocal_set "CONFIG_FOO=n"\n'
        )
        lines = [l for l in klocal.read_text().splitlines() if not l.startswith("#")]
        self.assertEqual(
            lines,
            [
                "CONFIG_NR_CPUS=8",
                "CONFIG_HEX=0x1f",
                'CONFIG_STR="-l2"',
                "CONFIG_FOO=n",
            ],
        )

    def test_klocal_set_rejects_injection(self):
        klocal = self.scratch / "kernel.local"
        pwn = self.scratch / "pwned"
        bad_inputs = [
            f"CONFIG_FOO/e touch {pwn};/=y",
            "CONFIG_FOO;q=y",
            "CONFIG_.*=y",
            "NOT_CONFIG=y",
            "CONFIG_FOO=",
            "CONFIG_FOO=$(touch " + str(pwn) + ")",
            "CONFIG_FOO=y\nCONFIG_BAR=y",
            'CONFIG_FOO="bad\\quote"',
        ]
        for bad in bad_inputs:
            self.run_l2_snippet(
                f'KLOCAL="{klocal}"\nklocal_set "$1"',
                env=dict(self.env),
                ok=False,
            ) if False else None
            r = subprocess.run(
                [
                    "bash",
                    "-Eeuo",
                    "pipefail",
                    "-c",
                    f'source "{self.l2_lib}"\nKLOCAL="{klocal}"\nklocal_set "$1"',
                    "_",
                    bad,
                ],
                env=self.env,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertNotEqual(r.returncode, 0, f"accepted bad input: {bad!r}")
        self.assertFalse(pwn.exists(), "injection executed command")

    # -------------------------------------------------------- download()
    def _make_curl_shim(self, payload: bytes):
        shimdir = self.scratch / "shimbin"
        shimdir.mkdir(exist_ok=True)
        args_log = self.scratch / "curl.args"
        payload_file = self.scratch / "curl.payload"
        payload_file.write_bytes(payload)
        curl = shimdir / "curl"
        curl.write_text(
            "#!/bin/sh\n"
            f'printf "%s\\n" "$@" > "{args_log}"\n'
            'out=""\n'
            'while [ $# -gt 0 ]; do\n'
            '  if [ "$1" = "-o" ]; then out=$2; shift 2; continue; fi\n'
            '  shift\n'
            'done\n'
            f'cp "{payload_file}" "$out"\n'
        )
        curl.chmod(0o755)
        env = dict(self.env, PATH=f"{shimdir}:{self.env['PATH']}")
        return env, args_log

    def test_download_rejects_http(self):
        dest = self.scratch / "cache" / "file.tar.gz"
        r = self.run_l2_snippet(
            f'download "http://example.com/file.tar.gz" "{dest}" "" "test"',
            ok=False,
        )
        self.assertIn("non-HTTPS", r.stderr + r.stdout)
        self.assertFalse(dest.exists())

    def test_download_sha_verified_before_mv(self):
        payload = b"malicious payload\n"
        env, args_log = self._make_curl_shim(payload)
        dest = self.scratch / "cache" / "artifact.tar.gz"
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(b"original-good-content\n")
        want_sha = "0" * 64
        r = self.run_l2_snippet(
            f'download "https://example.com/artifact.tar.gz" "{dest}" "{want_sha}" "test"',
            env=env,
            ok=False,
        )
        self.assertIn("sha256 mismatch", r.stderr + r.stdout)
        # Original dest must NOT be overwritten with unverified payload
        self.assertEqual(dest.read_bytes(), b"original-good-content\n")
        # No stray .part files left behind
        parts = list(dest.parent.glob("artifact.tar.gz.part*"))
        self.assertEqual(parts, [])
        # curl must enforce https + tlsv1.2
        curl_args = args_log.read_text().splitlines()
        self.assertIn("--proto", curl_args)
        self.assertIn("=https", curl_args)
        self.assertIn("--tlsv1.2", curl_args)

    def test_download_sha_match_commits_atomically(self):
        payload = b"verified release archive\n"
        want_sha = hashlib.sha256(payload).hexdigest()
        env, _ = self._make_curl_shim(payload)
        dest = self.scratch / "cache" / "good.tar.gz"
        self.run_l2_snippet(
            f'download "https://example.com/good.tar.gz" "{dest}" "{want_sha}" "good"',
            env=env,
            ok=True,
        )
        self.assertEqual(dest.read_bytes(), payload)
        self.assertEqual(list(dest.parent.glob("good.tar.gz.part*")), [])

    # -------------------------------------------------------- Phase-2 pack pin
    def test_write_pack_manifest_and_pin_deterministic(self):
        isod = self.scratch / "isod"
        hw = isod / "l2" / "hw"
        (hw / "lib" / "modules").mkdir(parents=True)
        (hw / "lib" / "modules" / "b.ko").write_bytes(b"mod-b")
        (hw / "lib" / "modules" / "a.ko").write_bytes(b"mod-a")
        ssl = isod / "l2" / "ssl"
        (ssl / "bin").mkdir(parents=True)
        (ssl / "bin" / "openssl").write_bytes(b"fake-openssl")

        pin1 = self.scratch / "pack-hash-1"
        pin2 = self.scratch / "pack-hash-2"
        self.run_l2_snippet(
            f'write_pack_manifest "{hw}"\n'
            f'write_pack_manifest "{ssl}"\n'
            f'write_pack_pin "{isod}" "{pin1}"\n'
            f'write_pack_manifest "{hw}"\n'
            f'write_pack_manifest "{ssl}"\n'
            f'write_pack_pin "{isod}" "{pin2}"\n'
        )
        hw_mf = (hw / "pack-manifest").read_text()
        expected_a = hashlib.sha256(b"mod-a").hexdigest()
        expected_b = hashlib.sha256(b"mod-b").hexdigest()
        self.assertEqual(
            hw_mf,
            f"{expected_a}  lib/modules/a.ko\n{expected_b}  lib/modules/b.ko\n",
        )
        self.assertEqual(pin1.read_bytes(), pin2.read_bytes())
        expected_hw_pin = hashlib.sha256(hw_mf.encode()).hexdigest()
        expected_ssl_pin = hashlib.sha256((ssl / "pack-manifest").read_bytes()).hexdigest()
        self.assertEqual(
            pin1.read_text(),
            f"{expected_hw_pin}  hw\n{expected_ssl_pin}  ssl\n",
        )

    def test_write_pack_manifest_rejects_symlinks(self):
        pack = self.scratch / "badpack"
        pack.mkdir()
        (pack / "real").write_text("ok")
        (pack / "evil").symlink_to("/etc/passwd")
        r = self.run_l2_snippet(f'write_pack_manifest "{pack}"', ok=False)
        self.assertIn("non-regular", r.stderr + r.stdout)

    # -------------------------------------------------------- l2fetch
    def test_l2fetch_safe_os_release_parsing_and_esc_strip(self):
        pwn = self.scratch / "l2fetch_pwn"
        osrel = self.scratch / "os-release"
        osrel.write_text(
            f'PRETTY_NAME="Evil\x1b[31mOS$(touch {pwn})"\n'
            f'touch "{pwn}"\n'
            'VERSION_ID="1.0"\n'
        )
        env = dict(self.env, L2FETCH_OS_RELEASE=str(osrel))
        r = subprocess.run(
            ["sh", str(L2FETCH), "--no-color", "--no-logo"],
            env=env,
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(pwn.exists(), "l2fetch executed code from os-release")
        self.assertNotIn("\x1b", r.stdout, "l2fetch leaked ESC into output")
        self.assertIn("Evil[31mOS", r.stdout)


if __name__ == "__main__":
    unittest.main()
