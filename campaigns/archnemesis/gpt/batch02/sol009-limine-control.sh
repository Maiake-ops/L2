#!/usr/bin/env bash
# Run twice with the same scratch path and different new evidence paths.
set -euo pipefail

if [[ $# != 3 || ! -f sources.lock || ! -x tools/l2-lowram ]]; then
    echo "usage from repository root: bash $0 ORIGINAL_BUILD_DIR FIXED_SCRATCH_DIR NEW_EVIDENCE_DIR" >&2
    exit 2
fi
original=$(realpath "$1")
[[ ! -e $3 ]]
mkdir -- "$2"
scratch=$(realpath "$2")
ver=$(sed -n 's/^limine_version=//p' sources.lock)
hash=$(sed -n 's/^limine_sha256=//p' sources.lock)

mkdir -p "$scratch/core/cache" "$scratch/lowram/out" "$scratch/lowram/logs"
archive="$scratch/core/cache/limine-$ver.tar.gz"
cp -- "$original/core/cache/limine-$ver.tar.gz" "$archive"
printf '%s  %s\n' "$hash" "$archive" | sha256sum -c -
cp -- "$original/lowram/out/"{vmlinuz-l2-lowram,initramfs-l2-lowram} "$scratch/lowram/out/"

SOURCE_DATE_EPOCH=$(sed -n 's/^epoch=//p' sources.lock)
[[ $SOURCE_DATE_EPOCH =~ ^[0-9]+$ ]]
export SOURCE_DATE_EPOCH JOBS=${JOBS:-3} L2_LOAD=${L2_LOAD:-8} L2_PLAIN=1 L2_ROOT="$PWD"
L2_BUILD_DIR="$scratch/core" ./l2 build limine
L2_BUILD_DIR="$scratch/lowram" L2_MAIN_BUILD_DIR="$scratch/core" tools/l2-lowram iso
mv -- "$scratch" "$3"
sha256sum "$3/lowram/out/l2-lowram.iso"
