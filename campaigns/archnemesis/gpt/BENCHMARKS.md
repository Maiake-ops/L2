# Second Front batch 1 benchmarks

Baseline source was `380a3651e`. The clean checkout failed its release-verifier regression because the documented script was absent. The saved primary checkout had that ignored file, which explains the passing check recorded in Gemini's baseline. We separately pinned copies of the pre-existing images, then assembled a matched before-and-after pair with `SOURCE_DATE_EPOCH` set to the baseline commit time. The candidate core rebuild reused the hardware kernel and cached tools. The candidate low-RAM kernel was compiled from the Linux 7.3.3 archive after validating its source-lock digest. The enabled Kconfig values matched the pinned low-RAM baseline.

| Artifact | Matched baseline | Candidate | Candidate SHA-256 |
| --- | ---: | ---: | --- |
| Core ISO | 8,636,416 B | 8,636,416 B (8.24 MiB) | `35b21d49d7e3291b47f8dc55f9b89e353bcca137007ae59798bd51c01bfcfd16` |
| Core compressed kernel | 2,831,360 B | 2,831,360 B (2.70 MiB) | `4848e46fdec98de75809600e50f64e6b41b2fe41ec84f58a20a1bedd16dafd98` |
| Low-RAM ISO | 5,967,872 B | 5,967,872 B (5.69 MiB) | `81deafab6656f4da35b6539b496e8887de82512093000baa754b2112f2ba5fb5` |
| Low-RAM compressed kernel | 1,647,616 B | 1,647,616 B (1.57 MiB) | `dde1abd4b7221c4a415dd94a89577b2985a1edf856c2c55227defbcb7b0e53f5` |

Both candidates matched their own repeated ISO assemblies byte for byte. This check covers ISO assembly with the same inputs. The low-RAM Linux ELF was 5,775,040 bytes on disk. Its boot header advertised 4,976,640 bytes for decompression and initialization. Actual peak boot memory was unavailable.

| Image and firmware | Configured RAM | Kernel-visible KiB | Configured minus visible KiB | Available KiB | Used KiB | PID 1 RSS KiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Matched core baseline BIOS | 256 MiB | 250,516 | 11,628 | 244,932 | 5,584 | 36 |
| Core candidate BIOS | 256 MiB | 250,516 | 11,628 | 244,896 | 5,620 | 32 |
| Core candidate UEFI | 256 MiB | 244,592 | 17,552 | 238,836 | 5,756 | 32 |
| Core candidate installed BIOS | 256 MiB | 250,504 | 11,640 | 245,356 | 5,148 | 32 |
| Matched low-RAM baseline BIOS | 9 MiB | 4,416 | 4,800 | 580 | 3,836 | 36 |
| Low-RAM candidate BIOS | 9 MiB | 4,416 | 4,800 | 568 | 3,848 | 40 |
| Low-RAM candidate BIOS | 10 MiB | 5,440 | 4,800 | 1,600 | 3,840 | 36 |
| Low-RAM candidate UEFI | 128 MiB | 118,500 | 12,572 | 114,520 | 3,980 | 36 |

QEMU used KVM, two virtual CPUs and a 40-second settle. Used memory is guest `MemTotal - MemAvailable` from the lowest of three settled samples. Configured minus visible is a derived reservation figure that includes firmware and other unavailable pages. At 9 MiB the candidate sample reported 760 KiB free, 0 KiB reclaimable slab, 2,032 KiB unreclaimable slab and 836 KiB cached. The 9 MiB candidate answered the shell command and powered off with QEMU status 0. Both the saved and candidate profiles failed to reach the shell at 8 MiB. No lower UEFI floor was tested. The measured RAM variation is insufficient to claim a performance improvement.

## Commands and evidence

- `make -j2 test` returned 0 with 241 Python tests, five native rcl2 regression binaries, and the configured self-tests. The low-RAM Limine menu regression ran.
- `python3 tools/verify-lowram.py <candidate-iso> <candidate-sha256>` returned 0. It ran `make check`, booted the pinned snapshot under BIOS at 10 MiB and UEFI at 128 MiB, checked PID 1 RSS and the shell, and observed a successful poweroff in both guests.
- `python3 tools/ramtest.py --iso <candidate-iso> --ram 9 --settle 40 --timeout 80 --budget 10240 --pid1-budget 50` verified the 9 MiB run. The diagnostic captured the selected `/proc/meminfo` sample.
- `python3 tools/lowram_matrix.py --iso <candidate-iso> --sizes 8M --modes bios --smp 2 --settle 40 --timeout 40 --outdir <evidence-dir>` returned 1 at 8 MiB with no shell. The exit of the VM process alone is insufficient to establish boot success.
- The core live image installed onto a disposable virtual disk and powered off. A fresh boot verified the ext4 root, guest shell and clean poweroff. The temporary disk was removed.
- The stability screen passed two 256 MiB boots, a short soak and ext4 post-crash repair checks. It did not meet or claim the endurance gate.
