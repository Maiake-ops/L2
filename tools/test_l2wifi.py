"""Regression tests for l2wifi control-socket client and security guards."""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import unittest

REPO = Path(__file__).resolve().parent.parent


class MockSupplicant:
    def __init__(self, ctrl_dir):
        self.sock_path = ctrl_dir / "lo"
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind(str(self.sock_path))
        self.sock.settimeout(0.2)
        self.commands = []
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        while not self.stop_event.is_set():
            try:
                data, addr = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            cmd = data.decode(errors="replace").strip()
            self.commands.append(cmd)
            if cmd == "STATUS":
                reply = "wpa_state=COMPLETED\nssid=Home\\x20Net\nfreq=5180\nkey_mgmt=WPA2-PSK\n"
            elif cmd == "SIGNAL_POLL":
                reply = "RSSI=-48\n"
            elif cmd == "SCAN":
                reply = "OK\n"
            elif cmd == "SCAN_RESULTS":
                reply = (
                    "bssid / frequency / signal level / flags / ssid\n"
                    "00:11:22:33:44:55\t5180\t-45\t[WPA2-PSK-CCMP][ESS]\tHome\\x20Net\n"
                    "aa:bb:cc:dd:ee:ff\t2412\t-60\t[ESS]\tCafe\n"
                )
            elif cmd == "LIST_NETWORKS":
                reply = "network id / ssid / bssid / flags\n0\tHome\\x20Net\tany\t[CURRENT]\n"
            elif cmd == "ADD_NETWORK":
                reply = "1\n"
            elif cmd.startswith(("SET_NETWORK ", "ENABLE_NETWORK ", "SELECT_NETWORK ",
                                 "REMOVE_NETWORK ", "SET country ", "SAVE_CONFIG",
                                 "DISCONNECT", "RECONNECT")):
                reply = "OK\n"
            else:
                reply = "FAIL\n"
            self.sock.sendto(reply.encode(), addr)

    def close(self):
        self.stop_event.set()
        self.thread.join(timeout=2)
        self.sock.close()


class L2wifiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", str(REPO / "l2wifi")], check=True, capture_output=True)
        cls.bin = REPO / "l2wifi/l2wifi"

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="l2wifi-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.ctrl = self.tmp / "ctrl"
        self.ctrl.mkdir()
        self.server = MockSupplicant(self.ctrl)
        self.addCleanup(self.server.close)
        self.env = dict(
            os.environ,
            L2WIFI_CTRL_DIR=str(self.ctrl),
            L2WIFI_NET="/bin/true",
            NO_COLOR="1",
        )

    def run_wifi(self, *args, ok=True):
        r = subprocess.run(
            [str(self.bin), "--no-color", "-i", "lo", *args],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=15,
        )
        if ok:
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        else:
            self.assertNotEqual(r.returncode, 0, f"expected failure: {r.stdout}\n{r.stderr}")
        return r

    def test_status_list_forget_and_socket_cleanup(self):
        r = self.run_wifi("status")
        self.assertIn("Home Net", r.stdout)
        self.assertIn("127.0.0.1", r.stdout)
        r = self.run_wifi("list")
        self.assertIn("Home Net", r.stdout)
        self.assertIn("(connected)", r.stdout)
        r = self.run_wifi("forget", "Home Net")
        self.assertIn("forgot Home Net", r.stdout)
        self.assertIn("REMOVE_NETWORK 0", self.server.commands)
        # No leaked client socket files in ctrl_dir.
        leftovers = [p.name for p in self.ctrl.iterdir() if p.name.startswith("l2wifi.")]
        self.assertEqual(leftovers, [], f"client socket leaked: {leftovers}")

    def test_connect_escapes_psk_and_rejects_control_chars(self):
        r = self.run_wifi("connect", "TestNet", 'pa"ss\\word1')
        self.assertIn("connected", r.stdout)
        self.assertTrue(
            any(c == 'SET_NETWORK 1 psk "pa\\"ss\\\\word1"' for c in self.server.commands),
            self.server.commands,
        )
        r = self.run_wifi("connect", "TestNet", "bad\npass123", ok=False)
        self.assertIn("control characters", r.stderr)

    def test_country_validation_and_uppercase(self):
        r = self.run_wifi("country", "th")
        self.assertIn("SET country TH", self.server.commands)
        self.assertIn("OK", r.stdout)
        for bad in ("12", "a;", "USA", "t"):
            self.run_wifi("country", bad, ok=False)

    def test_bad_interface_rejected(self):
        r = subprocess.run(
            [str(self.bin), "-i", "bad;iface", "status"],
            env=self.env,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("bad interface", r.stderr)


if __name__ == "__main__":
    unittest.main()
