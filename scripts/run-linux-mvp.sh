#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${1:-"/boot/vmlinuz-$(uname -r)"}
initramfs=${2:-"$project_dir/build/initramfs.cpio.gz"}

if [ ! -r /dev/kvm ]; then
    echo "error: /dev/kvm is not readable" >&2
    exit 1
fi
if [ ! -r "$kernel" ]; then
    echo "error: kernel not readable: $kernel" >&2
    exit 1
fi

if [ ! -r "$initramfs" ]; then
    "$project_dir/scripts/build-initramfs.sh" "$initramfs"
fi
make -C "$project_dir"
exec "$project_dir/agent-cell" kvm-boot "$kernel" "$initramfs"
