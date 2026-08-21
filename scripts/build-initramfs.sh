#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-"$project_dir/build/initramfs.cpio.gz"}
build_dir=${BUILD_DIR:-"$project_dir/build/guest"}
cc=${CC:-cc}

mkdir -p "$build_dir/root/dev" "$build_dir/root/proc" "$build_dir/root/tmp"
"$cc" -static -Os -std=c11 -Wall -Wextra -Werror \
    -fno-ident -fno-asynchronous-unwind-tables \
    -o "$build_dir/root/init" "$project_dir/guest/init.c"
"$cc" -static -Os -std=c11 -Wall -Wextra -Werror \
    -fno-ident -fno-asynchronous-unwind-tables \
    -o "$build_dir/root/agent-task" "$project_dir/guest/agent-task.c"

mkdir -p "$(dirname -- "$output")"
(cd "$build_dir/root" && find . -print | LC_ALL=C sort | cpio -o -H newc 2>/dev/null) \
    | gzip -n -9 > "$output"
printf 'initramfs=%s\n' "$output"
