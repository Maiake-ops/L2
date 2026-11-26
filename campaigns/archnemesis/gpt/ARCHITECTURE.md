# L2 architecture relevant to Second Front

The core and low-RAM builds use the same x86_64 source lock. The core builder stages an initramfs and combines it with a hardware-capable kernel and on-medium install and hardware packs. Limine boots the resulting image under BIOS or UEFI. The low-RAM builder uses `config/kernel.lowram` and a serial console. It builds without modules, SMP, virtual terminals, IP networking or ext4. The low-RAM image supports a live serial shell. The core image provides the ext4 install path.

Both builders apply `tools/patch-kernel-ram.py` to a freshly extracted source tree. The patcher now validates all source substitutions before handing any changed contents to the filesystem. The low-RAM builder stops if a required configuration command fails. The extracted kernel tree, logs and images are kept under a private build directory during this campaign.

rcl2 is the first userspace process. It mounts the early filesystems, starts services from `rcl2.conf`, reaps and respawns children, and handles shutdown requests. The boot checks query `/proc/1/status` for its settled resident memory. The installer consumes the live media's install pack, prepares a target disk and installs a bootable ext4 root. SIGINT and SIGTERM now exit through the active cleanup handler before further installer commands can run.

l2pm holds package store and profile state in the guest root. The batch ran the existing package tests. Work directly on package transactions, service supervision and desktop packaging remains subject to the ownership recorded in [CONFLICTS.md](CONFLICTS.md).

`tools/ramtest.py` performs a live shell probe and requires a successful QEMU exit after poweroff. `tools/lowram_matrix.py` brackets supported guest memory sizes and records serial evidence. `tools/test-stability.py` now requires an initial successful ext4 scratch write before it simulates power loss.
