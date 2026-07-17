#!/usr/bin/env bash
set -euo pipefail

# Build the Minix hard-disk image that holds the userland tasks and docs.
#
# WARNING: this script needs sudo because it mounts a loopback device. Unlike
# the old version it no longer hardcodes /dev/loop0 or /mnt: it attaches the
# image to the first free loop device (losetup --find) and mounts it on a
# private temporary directory, cleaning both up on exit even on failure.

cd "$(dirname "$0")"

IMG=hdd.img
MNT="$(mktemp -d)"
LOOP=""

cleanup() {
    if mountpoint -q "$MNT"; then
        sudo umount "$MNT" || true
    fi
    if [[ -n "$LOOP" ]]; then
        sudo losetup -d "$LOOP" || true
    fi
    rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

# Step 1: create the raw 16 MiB image (formerly Bochs' bximage).
truncate -s 16M "$IMG"
# Step 2: create the Minix filesystem on it.
mkfs -V -t minix "$IMG"
# Step 3: attach it to a free loop device and mount it.
LOOP="$(sudo losetup --find --show "$IMG")"
sleep 2
sudo mount "$LOOP" "$MNT"
# Step 4: populate the directory tree.
sudo cp -r docs "$MNT"
make -C tasks/
sudo mkdir "$MNT/tasks"
sudo cp tasks/*.run "$MNT/tasks"
sudo mkdir "$MNT/dev"
sudo mknod "$MNT/dev/tty" c 0 0
# Step 5: flush and detach explicitly (cleanup would also handle this).
sync
sudo umount "$MNT"
sudo losetup -d "$LOOP"
LOOP=""
# Step 6: move the finished image into place.
mv "$IMG" ../hdd.img
