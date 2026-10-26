#!/usr/bin/env bash
# make-disk.sh — pack the built rootfs into a test disk image (needs sudo)
#   sudo ./make-disk.sh
set -euo pipefail

JAI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
R="$JAI_ROOT/build/rootfs"
IMG="$JAI_ROOT/vm/jai-test.img"

[ -d "$R/boot" ] || { echo "ERROR: no rootfs at $R — run ./build.sh rootfs && sudo ./setup-rootfs.sh first" >&2; exit 1; }
[ -f "$R/boot/vmlinuz" ] || { echo "ERROR: no kernel in rootfs /boot" >&2; exit 1; }

echo "[jai] creating 6 GB sparse disk image at $IMG"
rm -f "$IMG"
truncate -s 6G "$IMG"

echo "[jai] formatting ext4 and populating directly from rootfs (no mount needed)"
mkfs.ext4 -q -F -L jairoot -E root_owner=0:0 -d "$R" "$IMG"

echo "[jai] DONE — boot with ./boot-vm.sh"
