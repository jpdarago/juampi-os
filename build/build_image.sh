#!/usr/bin/env bash
set -euo pipefail

# Build the Minix hard-disk image with the userland tasks and docs — entirely
# from userspace. No sudo, no loopback mount: mkfs.minix lays down an empty
# filesystem (it works on a plain file), and mkminixfs copies files in by
# writing the on-disk structures directly. This also makes the image buildable
# on macOS and in CI without privileges.

cd "$(dirname "$0")"

IMG=hdd.img
HOST_CC="${HOST_CC:-cc}"

# Step 1: create an empty 16 MiB Minix v1 filesystem (magic 0x138F).
truncate -s 16M "$IMG"
mkfs.minix "$IMG"

# Step 2: build the userland tasks and the populate tool.
make -C tasks/
"$HOST_CC" -O2 -Wall -o mkminixfs mkminixfs.c

# Step 3: populate the image from a manifest (no mount).
{
    echo "D /tasks"
    for run in tasks/*.run; do
        echo "F /tasks/$(basename "$run") $run"
    done
    echo "D /docs"
    echo "F /docs/prueba1.txt docs/prueba1.txt"
    echo "D /docs/docs"
    echo "F /docs/docs/prueba2.txt docs/docs/prueba2.txt"
    echo "F /docs/docs/prueba3.txt docs/docs/prueba3.txt"
    echo "D /dev"
    echo "C /dev/tty 0 0"
} | ./mkminixfs "$IMG"

# Step 4: move the finished image into place.
mv "$IMG" ../hdd.img
