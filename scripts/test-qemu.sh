#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${1:?"usage: test-qemu.sh BZIMAGE [INITRAMFS]"}
initramfs=${2:-"$project_dir/build/initramfs.cpio.gz"}
log=${QEMU_LOG:-"$project_dir/build/qemu-boot.log"}

if [ ! -r "$initramfs" ]; then
    "$project_dir/scripts/build-initramfs.sh" "$initramfs"
fi
mkdir -p "$(dirname -- "$log")"

set +e
timeout 10 qemu-system-x86_64 \
    -accel tcg -machine microvm -cpu max -m 128M -smp 1 \
    -nodefaults -no-reboot -nographic -serial stdio \
    -kernel "$kernel" -initrd "$initramfs" \
    -append "console=ttyS0,115200 panic=1 reboot=k rdinit=/init pci=off acpi=off" \
    >"$log" 2>&1
qemu_status=$?
set -e

if ! grep -q 'AGENT_TASK_OK' "$log" || ! grep -q 'CELL_MVP_OK' "$log"; then
    tail -80 "$log" >&2
    echo "qemu-mvp=failed qemu_status=$qemu_status" >&2
    exit 1
fi
echo "qemu-mvp=ok log=$log"
