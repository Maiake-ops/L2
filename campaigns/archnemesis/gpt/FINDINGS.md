# Second Front findings

## SOL-001 and SOL-002

Bash suppresses automatic exit on error inside the condition used by the low-RAM configure step. A later successful `scripts/config` command hid four simulated failures and let the builder publish the fixture image. An explicit success chain preserves the first failing command. The existing fragment-merge fallback still handles its original compatibility case.

The RAM patcher used to write each file immediately. A late hunk mismatch left earlier files changed and caused the next attempt to fail at an already-applied substitution. The patcher now finishes validation before writing. An I/O error during the subsequent writes can still leave partial output. Both build paths extract a fresh source tree on their next build.

## SOL-003

The disk and image installers set the same trap for cleanup and interruption. The trap returned to the install function after SIGINT or SIGTERM and the disk dry-run continued to partitioning. The signal handlers now exit with status 130 or 143 so the existing exit cleanup executes. A simulated image install released its loop device once. The tests cover the interval after cleanup is registered and after the simulated loop device is recorded. A signal during resource acquisition before ownership is recorded remains unverified.

## SOL-004

The crash-recovery test announced that writes were active even when entering the target filesystem or the first write had failed. The guest command now requires both to succeed before the host requests a crash. Controlled regressions cover both failure paths and marker emission on success. A short QEMU run exercised ext4 writes, repair and post-crash writes.

## SOL-005

A Git ignore rule excluded the low-RAM release verifier. Its documentation and negative tests were tracked, so a clean checkout could neither run the screen nor pass `make check`. Tracking the existing verifier restores the documented checks. The verifier pins an ISO by its SHA-256 digest, which establishes artifact identity when the expected digest is trusted.

## Remaining work

The low-RAM matrix currently sends a poweroff command and can report a passing boot without observing process exit. The dedicated release gate does wait for successful exit. The default kernel builder has a separate configure sequence that needs its failure handling checked. Atomic ISO publication and installer resource acquisition still need focused review. This batch did not assess attack behavior or change package or PID 1 security controls.
