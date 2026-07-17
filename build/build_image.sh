#!/usr/bin/env bash
set -euo pipefail

# Build the ext2 hard-disk image with the userland tasks and docs — entirely
# from userspace. No sudo, no loopback mount: mke2fs creates the filesystem on
# a plain file, e2tools copies files/dirs in, and debugfs adds the /dev/tty
# node. All of this works on Linux and macOS.

cd "$(dirname "$0")"

IMG=hdd.img

# 16 MiB, 1024-byte blocks so the on-disk block size matches the kernel's
# buffer cache and ext2 driver. Disable resize_inode/dir_index to keep the
# on-disk layout simple for the read-only driver.
rm -f "$IMG"
mke2fs -q -F -t ext2 -b 1024 -I 128 -O ^resize_inode,^dir_index "$IMG" 16384

make -C tasks/

e2mkdir "$IMG:/tasks"
for run in tasks/*.run; do
    e2cp "$run" "$IMG:/tasks/$(basename "$run")"
done

e2mkdir "$IMG:/docs"
e2cp docs/prueba1.txt "$IMG:/docs/prueba1.txt"
e2mkdir "$IMG:/docs/docs"
e2cp docs/docs/prueba2.txt "$IMG:/docs/docs/prueba2.txt"
e2cp docs/docs/prueba3.txt "$IMG:/docs/docs/prueba3.txt"

e2mkdir "$IMG:/dev"
# debugfs only links the node when created relative to the parent dir, so cd
# in first (the absolute-path form allocates an inode but leaves it unlinked).
printf 'cd /dev\nmknod tty c 0 0\n' | debugfs -w "$IMG" >/dev/null 2>&1

mv "$IMG" ../hdd.img
