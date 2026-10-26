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

# The rootfs is live — evict any mounted virtual filesystems first,
# otherwise mkfs -d copies the host's dynamic /proc /sys /dev state.
echo "[jai] unmounting virtual filesystems from rootfs"
for m in sys dev proc run; do
	umount -R -f "$R/$m" 2>/dev/null || umount -R "$R/$m" 2>/dev/null || true
done
# rbind created stacked mounts; repeat until clean
for _ in 1 2 3 4 5; do
	grep -q "jai-os/build/rootfs" /proc/mounts || break
	for m in sys dev proc run; do umount -R "$R/$m" 2>/dev/null || true; done
done
if grep -q "jai-os/build/rootfs" /proc/mounts; then
	echo "ERROR: could not unmount all virtual filesystems from rootfs" >&2
	grep "jai-os/build/rootfs" /proc/mounts >&2
	exit 1
fi
echo "[jai]   clean"

echo "[jai] formatting ext4 and populating directly from rootfs (no mount needed)"
mkfs.ext4 -q -F -L jairoot -E root_owner=0:0 -d "$R" "$IMG"

echo "[jai] DONE — boot with ./boot-vm.sh"
