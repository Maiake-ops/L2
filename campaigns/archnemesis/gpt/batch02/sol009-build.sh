#!/usr/bin/env bash
# Run from a clean campaign checkout with pinned component source archives.
set -euo pipefail

if [[ $# != 2 ]] || [[ ! -f sources.lock || ! -x tools/l2-lowram ]]; then
    echo "usage from repository root: bash $0 SOURCE_ARCHIVE_DIR NEW_BUILD_DIR" >&2
    exit 2
fi
if [[ -n $(git status --porcelain) ]]; then
    echo "source checkout must be clean" >&2
    exit 2
fi
archives=$(realpath "$1")
mkdir -- "$2"
out=$(realpath "$2")
mkdir -p "$out/core/cache"
for entry in linux:kernel:tar.xz musl:musl:tar.gz busybox:busybox:tar.bz2 dash:dash:tar.gz limine:limine:tar.gz; do
    IFS=: read -r project key extension <<< "$entry"
    ver=$(sed -n "s/^${key}_version=//p" sources.lock)
    hash=$(sed -n "s/^${key}_sha256=//p" sources.lock)
    archive="$project-$ver.$extension"
    [[ -n $ver && ${#hash} == 64 ]]
    cp -- "$archives/$archive" "$out/core/cache/$archive"
    printf '%s  %s\n' "$hash" "$out/core/cache/$archive" | sha256sum -c -
done

git rev-parse HEAD
SOURCE_DATE_EPOCH=$(sed -n 's/^epoch=//p' sources.lock)
[[ $SOURCE_DATE_EPOCH =~ ^[0-9]+$ ]]
export SOURCE_DATE_EPOCH
export JOBS=${JOBS:-3} L2_LOAD=${L2_LOAD:-8} L2_PLAIN=1
export L2_ROOT="$PWD"
L2_BUILD_DIR="$out/core" ./l2 build musl headers busybox dash rcl2 l2pm l2wifi limine rootfs initramfs
L2_BUILD_DIR="$out/lowram" L2_MAIN_BUILD_DIR="$out/core" tools/l2-lowram all
sha256sum "$out/lowram/out/"{vmlinuz-l2-lowram,initramfs-l2-lowram,l2-lowram.iso}
