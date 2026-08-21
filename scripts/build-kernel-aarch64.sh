#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel_src=${1:?"usage: build-kernel-aarch64.sh LINUX_SOURCE_DIR [OUTPUT_IMAGE]"}
output=${2:-"$project_dir/build/Image-aarch64"}
object_dir=${KERNEL_BUILD_DIR:-"$project_dir/build/kernel-aarch64"}
stage=$(mktemp -d /tmp/agent-cell-kernel.XXXXXX)
trap 'rm -rf "$stage"' EXIT HUP INT TERM

if [ "$(uname -s)-$(uname -m)" != "Linux-aarch64" ]; then
    echo "error: kernel MVP build requires Linux aarch64" >&2
    exit 1
fi
if [ ! -x "$kernel_src/scripts/config" ]; then
    echo "error: invalid Linux source tree: $kernel_src" >&2
    exit 1
fi

"$project_dir/scripts/build-initramfs.sh" "$stage/initramfs.cpio.gz"
mkdir -p "$stage/root"
cp -a "$project_dir/build/guest/root/." "$stage/root/"

make -C "$kernel_src" O="$object_dir" tinyconfig
"$kernel_src/scripts/config" --file "$object_dir/.config" \
    --enable PRINTK --enable BUG --enable ELF_CORE \
    --enable BINFMT_ELF --enable BINFMT_SCRIPT \
    --enable BLK_DEV_INITRD --enable DEVTMPFS --enable DEVTMPFS_MOUNT \
    --enable PROC_FS --enable TMPFS --enable UNIX98_PTYS --enable TTY \
    --enable ARM_PSCI_FW --enable ARM_GIC --enable ARM_GIC_V3 \
    --enable SMP --enable POSIX_TIMERS --enable FHANDLE --enable EPOLL \
    --enable SIGNALFD --enable TIMERFD --enable SERIAL_EARLYCON \
    --enable SERIAL_AMBA_PL011 --enable SERIAL_AMBA_PL011_CONSOLE \
    --enable SERIAL_CORE --enable SERIAL_CORE_CONSOLE \
    --enable DEVMEM --disable STRICT_DEVMEM \
    --set-str INITRAMFS_SOURCE "$stage/root"
make -C "$kernel_src" O="$object_dir" olddefconfig
make -C "$kernel_src" O="$object_dir" -j"${JOBS:-$(getconf _NPROCESSORS_ONLN)}" Image

mkdir -p "$(dirname -- "$output")"
cp "$object_dir/arch/arm64/boot/Image" "$output"
dtc -I dts -O dtb -o "$project_dir/build/virt-aarch64.dtb" \
    "$project_dir/guest/virt-aarch64.dts"
printf 'kernel=%s\ndtb=%s\n' "$output" "$project_dir/build/virt-aarch64.dtb"
