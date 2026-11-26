"""Regression tests for l2gpt GPT parsing hardening.

All tests run against scratch image files (l2gpt accepts regular files),
never real devices.  Includes crafted corrupt/malicious GPT structures.
Offline and fast; wired into `make check`.
"""
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parent.parent
SECT = 512
IMG_SECTORS = 131072  # 64 MiB


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def fix_hdr_crc(img, lba):
    off = lba * SECT
    h = bytearray(img[off:off + 92])
    h[16:20] = b"\x00\x00\x00\x00"
    struct.pack_into("<I", h, 16, crc32(bytes(h)))
    img[off:off + 92] = h


def fix_entries_crc(img, hdr_lba, elba):
    start = elba * SECT
    arr = bytes(img[start:start + 128 * 128])
    off = hdr_lba * SECT
    struct.pack_into("<I", img, off + 88, crc32(arr))
    fix_hdr_crc(img, hdr_lba)


class L2gpt(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="l2gpt-test-")
        cls.bin = Path(cls.tmp) / "l2gpt"
        cc = os.environ.get("CC", "cc")
        r = subprocess.run([cc, "-std=c11", "-Os", "-Wall", "-Wextra",
                            "-o", str(cls.bin), str(REPO / "l2gpt" / "l2gpt.c")],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise unittest.SkipTest(f"cannot compile l2gpt: {r.stderr}")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.img = Path(self.tmp) / f"disk-{self._testMethodName}.img"
        with open(self.img, "wb") as f:
            f.truncate(IMG_SECTORS * SECT)

    def run_gpt(self, *args, ok=True):
        r = subprocess.run([str(self.bin), *args, str(self.img)],
                           capture_output=True, text=True, timeout=30)
        if ok:
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        else:
            self.assertNotEqual(r.returncode, 0,
                                f"expected failure, got success: {r.stdout}")
        return r

    def read_sect(self, lba):
        with open(self.img, "rb") as f:
            f.seek(lba * SECT)
            return f.read(SECT)

    def write_sect(self, lba, data):
        with open(self.img, "r+b") as f:
            f.seek(lba * SECT)
            f.write(data)

    def make_l2_layout(self):
        self.run_gpt("--zap-all")
        self.run_gpt("-n", "1:2048:+1M", "-t", "1:ef02", "-c", "1:L2-BIOS",
                     "-n", "2:0:+32M", "-t", "2:ef00",
                     "-n", "3:0:0", "-t", "3:8300")

    # ----------------------------------------------------------- basics
    def test_layout_and_verify(self):
        self.make_l2_layout()
        r = self.run_gpt("-v")
        self.assertIn("No problems found", r.stdout)
        r = self.run_gpt("-p")
        for name in ("L2-BIOS", "ef02", "ef00", "8300"):
            self.assertIn(name, r.stdout)

    def test_protective_mbr(self):
        self.make_l2_layout()
        mbr = self.read_sect(0)
        self.assertEqual(mbr[510:512], b"\x55\xaa")
        self.assertEqual(mbr[0x1BE + 4], 0xEE)
        self.assertEqual(struct.unpack_from("<I", mbr, 0x1BE + 8)[0], 1)

    def test_backup_matches_primary(self):
        self.make_l2_layout()
        prim = self.read_sect(2)[:128 * 128]
        back = self.read_sect(IMG_SECTORS - 33)[:128 * 128]
        self.assertEqual(prim, back)
        ph, bh = self.read_sect(1), self.read_sect(IMG_SECTORS - 1)
        self.assertEqual(ph[:8], b"EFI PART")
        self.assertEqual(bh[:8], b"EFI PART")
        self.assertEqual(struct.unpack_from("<Q", ph, 32)[0], IMG_SECTORS - 1)
        self.assertEqual(struct.unpack_from("<Q", bh, 32)[0], 1)

    def test_guid_and_name_roundtrip(self):
        self.make_l2_layout()
        self.run_gpt("-U", "12345678-1234-1234-1234-123456789abc",
                     "-u", "1:abcdef01-2345-6789-abcd-ef0123456789",
                     "-c", "1:Hello")
        r = self.run_gpt("-p")
        self.assertIn("12345678-1234-1234-1234-123456789ABC", r.stdout.upper())
        r = self.run_gpt("-i", "1")
        self.assertIn("ABCDEF01-2345-6789-ABCD-EF0123456789", r.stdout)
        self.assertIn("Partition name: 'Hello'", r.stdout)

    def test_zap_clears_both_headers(self):
        self.make_l2_layout()
        self.run_gpt("--zap-all")
        r = self.run_gpt("-v", ok=False)
        self.assertIn("primary GPT header invalid", r.stderr)
        self.assertIn("backup GPT header invalid", r.stderr)

    # ------------------------------------------------- corruption handling
    def test_corrupt_primary_falls_back_to_backup(self):
        self.make_l2_layout()
        h = bytearray(self.read_sect(1))
        h[16] ^= 0xFF  # break header CRC
        self.write_sect(1, bytes(h))
        r = self.run_gpt("-v", ok=False)
        self.assertIn("primary GPT header invalid", r.stderr)
        r = self.run_gpt("-p")  # loads via backup
        self.assertIn("L2-BIOS", r.stdout)

    def test_corrupt_primary_entries_falls_back_to_backup(self):
        self.make_l2_layout()
        e = bytearray(self.read_sect(2))
        e[60] ^= 0xFF
        self.write_sect(2, bytes(e))
        r = self.run_gpt("-v", ok=False)
        self.assertIn("primary entry array CRC mismatch", r.stderr)
        r = self.run_gpt("-p")
        self.assertIn("L2-BIOS", r.stdout)
        self.assertIn("falling back to backup GPT", r.stderr)

    def test_corrupt_entry_crc_rejected(self):
        self.make_l2_layout()
        e = bytearray(self.read_sect(2))
        e[60] ^= 0xFF
        self.write_sect(2, bytes(e))
        # Invalidate backup header so fallback cannot rescue the corrupt primary.
        self.write_sect(IMG_SECTORS - 1, b"\x00" * SECT)
        r = self.run_gpt("-p", ok=False)
        self.assertIn("CRC mismatch", r.stderr)

    def test_malicious_entry_array_lba_rejected(self):
        self.make_l2_layout()
        with open(self.img, "r+b") as f:
            data = bytearray(f.read())
            struct.pack_into("<Q", data, SECT + 72, IMG_SECTORS + 1000)  # elba past EOF
            fix_hdr_crc(data, 1)
            data[(IMG_SECTORS - 1) * SECT:IMG_SECTORS * SECT] = b"\x00" * SECT
            f.seek(0)
            f.write(data)
        r = self.run_gpt("-p", ok=False)
        self.assertIn("out of range", r.stderr)

    def test_malicious_entry_lba_range_rejected(self):
        self.make_l2_layout()
        with open(self.img, "r+b") as f:
            data = bytearray(f.read())
            # entry 0 end LBA -> absurd value, CRCs kept consistent
            struct.pack_into("<Q", data, 2 * SECT + 40, 1 << 63)
            fix_entries_crc(data, 1, 2)
            data[(IMG_SECTORS - 1) * SECT:IMG_SECTORS * SECT] = b"\x00" * SECT
            f.seek(0)
            f.write(data)
        r = self.run_gpt("-p", ok=False)
        self.assertIn("invalid LBA range", r.stderr)

    def test_unsupported_entry_size_rejected(self):
        self.make_l2_layout()
        with open(self.img, "r+b") as f:
            data = bytearray(f.read())
            struct.pack_into("<I", data, SECT + 84, 256)  # esz
            fix_hdr_crc(data, 1)
            data[(IMG_SECTORS - 1) * SECT:IMG_SECTORS * SECT] = b"\x00" * SECT
            f.seek(0)
            f.write(data)
        r = self.run_gpt("-p", ok=False)
        self.assertIn("not supported", r.stderr)

    def test_zap_all_succeeds_on_corrupt_and_hybrid_gpt(self):
        # 1) Corrupt entry array CRC + invalid backup
        self.make_l2_layout()
        e = bytearray(self.read_sect(2))
        e[60] ^= 0xFF
        self.write_sect(2, bytes(e))
        self.write_sect(IMG_SECTORS - 1, b"\x00" * SECT)
        self.run_gpt("--zap-all")
        self.assertEqual(self.read_sect(1), b"\x00" * SECT)

        # 2) Hybrid MBR + unsupported entry size + invalid backup, zap + recreate in one call
        self.make_l2_layout()
        with open(self.img, "r+b") as f:
            data = bytearray(f.read())
            # Add a second MBR entry (hybrid MBR)
            data[0x1BE + 16 + 4] = 0x83
            struct.pack_into("<I", data, SECT + 84, 256)
            fix_hdr_crc(data, 1)
            data[(IMG_SECTORS - 1) * SECT:IMG_SECTORS * SECT] = b"\x00" * SECT
            f.seek(0)
            f.write(data)
        self.run_gpt("--zap-all", "-n", "1:2048:+1M", "-t", "1:ef02", "-c", "1:L2-BIOS")
        r = self.run_gpt("-v")
        self.assertIn("No problems found", r.stdout)

    def test_random_guids_are_uuidv4(self):
        uuidv4_re = re.compile(
            r"^[0-9A-F]{8}-[0-9A-F]{4}-4[0-9A-F]{3}-[89AB][0-9A-F]{3}-[0-9A-F]{12}$"
        )
        self.make_l2_layout()
        r = self.run_gpt("-p")
        m = re.search(r"Disk identifier \(GUID\): ([0-9A-F-]+)", r.stdout)
        self.assertIsNotNone(m, r.stdout)
        self.assertRegex(m.group(1), uuidv4_re)
        for part in ("1", "2", "3"):
            ri = self.run_gpt("-i", part)
            mp = re.search(r"Partition unique GUID: ([0-9A-F-]+)", ri.stdout)
            self.assertIsNotNone(mp, ri.stdout)
            self.assertRegex(mp.group(1), uuidv4_re)

    def test_write_gpt_backup_before_primary_and_fsync(self):
        if shutil.which("strace") is None:
            self.skipTest("strace not available")
        self.run_gpt("--zap-all")
        r = subprocess.run(
            ["strace", "-e", "trace=pwrite64,fsync",
             str(self.bin), "-n", "1:2048:+1M", "-t", "1:ef02", str(self.img)],
            capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        events = []
        for line in r.stderr.splitlines():
            m = re.match(r"pwrite64\(\d+, .*?, 512, (\d+)\)", line)
            if m:
                events.append(("pwrite", int(m.group(1)) // SECT))
            elif line.startswith("fsync("):
                events.append(("fsync", None))
        fsync_idxs = [i for i, (k, _) in enumerate(events) if k == "fsync"]
        self.assertGreaterEqual(len(fsync_idxs), 2, f"expected >=2 fsyncs, got {events}")
        first_sync = fsync_idxs[0]
        before_sync = [lba for k, lba in events[:first_sync] if k == "pwrite"]
        after_sync = [lba for k, lba in events[first_sync + 1:fsync_idxs[-1]] if k == "pwrite"]
        # Backup entries + backup header must precede first fsync; primary + MBR must follow it.
        self.assertIn(IMG_SECTORS - 1, before_sync)
        self.assertTrue(all(lba >= IMG_SECTORS - 33 for lba in before_sync), before_sync)
        self.assertIn(0, after_sync)
        self.assertIn(1, after_sync)
        self.assertTrue(all(lba <= 33 for lba in after_sync), after_sync)

    def test_bad_header_version_rejected(self):
        self.make_l2_layout()
        with open(self.img, "r+b") as f:
            data = bytearray(f.read())
            struct.pack_into("<I", data, SECT + 8, 0x00020000)
            struct.pack_into("<I", data, (IMG_SECTORS - 1) * SECT + 8, 0x00020000)
            fix_hdr_crc(data, 1)
            fix_hdr_crc(data, IMG_SECTORS - 1)
            f.seek(0)
            f.write(data)
        # both headers now claim revision 2.0: no GPT found, empty table
        r = self.run_gpt("-p")
        self.assertNotIn("L2-BIOS", r.stdout)

    # ------------------------------------------------------ argument safety
    def test_overlap_rejected(self):
        self.run_gpt("--zap-all")
        self.run_gpt("-n", "1:2048:+1M")
        r = self.run_gpt("-n", "2:2048:+1M", ok=False)
        self.assertIn("overlaps", r.stderr)

    def test_bad_size_syntax_rejected(self):
        self.run_gpt("--zap-all")
        for bad in ("+12junk", "-5", "+", "12x"):
            r = self.run_gpt("-n", f"1:0:{bad}", ok=False)
            self.assertTrue("bad number" in r.stderr or "invalid" in r.stderr,
                            r.stderr)

    def test_partition_number_bounds(self):
        self.run_gpt("--zap-all")
        self.run_gpt("-n", "0:0:+1M", ok=False)
        self.run_gpt("-n", "129:0:+1M", ok=False)
        for bad in ("1abc:0:+1M", "-1:0:+1M", "abc:0:+1M"):
            r = self.run_gpt("-n", bad, ok=False)
            self.assertIn("bad partition number", r.stderr)
        self.run_gpt("-n", "1:0:+1M")
        r = self.run_gpt("-t", "1x:8300", ok=False)
        self.assertIn("bad partition number", r.stderr)
        r = self.run_gpt("-i", "1x", ok=False)
        self.assertIn("bad partition number", r.stderr)

    def test_zero_and_oversized_relative_partition(self):
        self.run_gpt("--zap-all")
        r = self.run_gpt("-n", "1:0:+0", ok=False)
        self.assertIn("cannot be zero", r.stderr)
        # A relative size larger than the disk caps safely to last usable sector.
        self.run_gpt("-n", "1:0:+100000M", "-t", "1:8300")
        r = self.run_gpt("-v")
        self.assertIn("No problems found", r.stdout)

    def test_too_small_image(self):
        with open(self.img, "wb") as f:
            f.truncate(SECT * 100)
        r = self.run_gpt("-p", ok=False)
        self.assertIn("too small", r.stderr)

    @unittest.skipIf(os.geteuid() == 0, "root ignores file permissions")
    def test_readonly_image(self):
        self.make_l2_layout()
        os.chmod(self.img, 0o444)
        try:
            r = self.run_gpt("-p")
            self.assertIn("L2-BIOS", r.stdout)
            r = self.run_gpt("--zap-all", ok=False)
            self.assertIn("read-only", r.stderr)
        finally:
            os.chmod(self.img, 0o644)


if __name__ == "__main__":
    unittest.main()