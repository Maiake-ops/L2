"""Static linter and invariant checks for L2 kernel config fragments (M12/M13).

Verifies syntax, absence of duplicate/conflicting symbols, compression and trim
settings, real-hardware essentials in config/kernel.fragment, and VM-profile
invariants in config/kernel.lowram.  Offline and fast; wired into `make check`.
"""
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parent.parent
HW_FRAG = REPO / "config" / "kernel.fragment"
LOWRAM_FRAG = REPO / "config" / "kernel.lowram"

ASSIGN_RE = re.compile(
    r"^(CONFIG_[A-Za-z0-9_]+)=(y|m|n|-?[0-9]+|0x[0-9a-fA-F]+|\"[^\"]*\")$"
)
NOT_SET_RE = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")


def parse_fragment(path: Path):
    """Parse a kernel config fragment; return (symbols_dict, duplicates, bad_lines)."""
    raw = path.read_bytes()
    if b"\r\n" in raw:
        raise AssertionError(f"{path}: contains CRLF line endings")
    symbols: dict[str, str] = {}
    duplicates: list[tuple[int, str, str, str]] = []
    bad_lines: list[tuple[int, str]] = []
    for lineno, line in enumerate(raw.decode("utf-8").splitlines(), 1):
        if line != line.rstrip(" \t"):
            bad_lines.append((lineno, f"trailing whitespace: {line!r}"))
            continue
        stripped = line.strip()
        if not stripped:
            continue
        m_not = NOT_SET_RE.match(stripped)
        if m_not:
            sym, val = m_not.group(1), "n"
        elif stripped.startswith("#"):
            continue
        else:
            code = re.sub(r"\s+#.*$", "", stripped)
            m = ASSIGN_RE.match(code)
            if not m:
                bad_lines.append((lineno, f"invalid syntax: {line!r}"))
                continue
            sym, val = m.group(1), m.group(2)
        if sym in symbols:
            duplicates.append((lineno, sym, symbols[sym], val))
        symbols[sym] = val
    return symbols, duplicates, bad_lines


class TestKernelConfig(unittest.TestCase):
    def test_fragments_syntax_and_no_duplicates(self):
        for frag in (HW_FRAG, LOWRAM_FRAG):
            with self.subTest(fragment=frag.name):
                self.assertTrue(frag.is_file(), f"missing {frag}")
                symbols, duplicates, bad_lines = parse_fragment(frag)
                self.assertEqual(bad_lines, [], f"syntax issues in {frag.name}: {bad_lines}")
                self.assertEqual(duplicates, [], f"duplicate symbols in {frag.name}: {duplicates}")
                self.assertGreater(len(symbols), 50)

    def test_default_kernel_compression_and_trims(self):
        syms, _, _ = parse_fragment(HW_FRAG)
        expected = {
            "CONFIG_KERNEL_XZ": "y",
            "CONFIG_BASE_SMALL": "y",
            "CONFIG_SLUB_TINY": "y",
            "CONFIG_CC_OPTIMIZE_FOR_SIZE": "y",
            "CONFIG_TRIM_UNUSED_KSYMS": "y",
            "CONFIG_DEBUG_INFO_NONE": "y",
            "CONFIG_KALLSYMS": "n",
            "CONFIG_FW_LOADER_COMPRESS_ZSTD": "n",
            "CONFIG_MSDOS_FS": "n",
            "CONFIG_ATA_GENERIC": "m",
            "CONFIG_ACPI_AC": "m",
            "CONFIG_ACPI_BATTERY": "m",
        }
        for sym, want in expected.items():
            self.assertEqual(syms.get(sym), want, f"{sym} expected {want}, got {syms.get(sym)}")

    def test_default_kernel_real_hardware_essentials_intact(self):
        syms, _, _ = parse_fragment(HW_FRAG)
        must_be_y = (
            # Arch & boot
            "CONFIG_64BIT", "CONFIG_X86_64", "CONFIG_SMP", "CONFIG_EFI",
            "CONFIG_EFI_STUB", "CONFIG_EFIVAR_FS", "CONFIG_RELOCATABLE",
            "CONFIG_BLK_DEV_INITRD", "CONFIG_RD_GZIP",
            # Real-HW & VM storage
            "CONFIG_BLK_DEV_SD", "CONFIG_BLK_DEV_SR", "CONFIG_SATA_AHCI",
            "CONFIG_ATA_PIIX", "CONFIG_NVME_CORE", "CONFIG_BLK_DEV_NVME",
            "CONFIG_VIRTIO_BLK", "CONFIG_USB_XHCI_HCD", "CONFIG_USB_XHCI_PCI",
            "CONFIG_USB_EHCI_HCD", "CONFIG_USB_STORAGE",
            "CONFIG_MMC", "CONFIG_MMC_BLOCK", "CONFIG_MMC_SDHCI_PCI",
            # Boot filesystems
            "CONFIG_EXT4_FS", "CONFIG_VFAT_FS", "CONFIG_ISO9660_FS",
            "CONFIG_TMPFS", "CONFIG_DEVTMPFS",
            # Real-HW keyboards & display
            "CONFIG_VT", "CONFIG_VT_CONSOLE", "CONFIG_SERIAL_8250",
            "CONFIG_SERIAL_8250_CONSOLE", "CONFIG_KEYBOARD_ATKBD",
            "CONFIG_SERIO_I8042", "CONFIG_USB_HID", "CONFIG_HID_GENERIC",
            "CONFIG_INPUT_EVDEV", "CONFIG_FB_VESA", "CONFIG_FB_EFI",
            "CONFIG_FRAMEBUFFER_CONSOLE", "CONFIG_FONT_8x16", "CONFIG_FONT_TER16x32",
            # Power / ACPI / RTC
            "CONFIG_PM", "CONFIG_SUSPEND", "CONFIG_ACPI", "CONFIG_ACPI_BUTTON",
            "CONFIG_ACPI_PROCESSOR", "CONFIG_RTC_DRV_CMOS",
            # Networking & safety
            "CONFIG_NET", "CONFIG_INET", "CONFIG_UNIX", "CONFIG_PACKET",
            "CONFIG_VIRTIO_NET", "CONFIG_BUG",
        )
        for sym in must_be_y:
            self.assertEqual(syms.get(sym), "y", f"real-HW essential {sym} must be =y")

        # Default kernel must never disable CPU mitigations or KASLR.
        self.assertNotEqual(syms.get("CONFIG_CPU_MITIGATIONS"), "n")
        self.assertNotEqual(syms.get("CONFIG_RANDOMIZE_BASE"), "n")

    def test_default_kernel_ondemand_drivers_remain_modular(self):
        syms, _, _ = parse_fragment(HW_FRAG)
        must_be_m = (
            "CONFIG_CFG80211", "CONFIG_MAC80211", "CONFIG_IWLWIFI",
            "CONFIG_RTW88", "CONFIG_RTW89", "CONFIG_E1000", "CONFIG_E1000E",
            "CONFIG_IGC", "CONFIG_R8169", "CONFIG_I2C_HID_ACPI",
            "CONFIG_HID_MULTITOUCH", "CONFIG_SOUND", "CONFIG_SND_HDA_INTEL",
        )
        for sym in must_be_m:
            self.assertEqual(syms.get(sym), "m", f"on-demand driver {sym} must be =m")

    def test_lowram_fragment_vm_invariants(self):
        syms, _, _ = parse_fragment(LOWRAM_FRAG)
        expected = {
            "CONFIG_KERNEL_XZ": "y",
            "CONFIG_BASE_SMALL": "y",
            "CONFIG_SLUB_TINY": "y",
            "CONFIG_SMP": "n",
            "CONFIG_NR_CPUS": "1",
            "CONFIG_MODULES": "n",
            "CONFIG_VT": "n",
            "CONFIG_SERIAL_8250": "y",
            "CONFIG_SERIAL_8250_CONSOLE": "y",
            "CONFIG_ACPI": "y",
            "CONFIG_BLK_DEV_INITRD": "y",
            "CONFIG_RD_GZIP": "y",
            "CONFIG_ISO9660_FS": "y",
            "CONFIG_ATA_PIIX": "y",
            "CONFIG_BLK_DEV_SR": "y",
        }
        for sym, want in expected.items():
            self.assertEqual(syms.get(sym), want, f"lowram {sym} expected {want}, got {syms.get(sym)}")


if __name__ == "__main__":
    unittest.main()
