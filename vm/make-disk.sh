#!/usr/bin/env bash
# make-disk.sh — pack the built rootfs into a test disk image (needs sudo)
#   sudo ./make-disk.sh
#
# Robust against live /proc /sys /dev mounts inside the rootfs:
# we rsync into a clean staging dir with -x (stay on one filesystem),
# so mounted virtual filesystems are skipped by definition.
set -euo pipefail

JAI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
R="$JAI_ROOT/build/rootfs"
IMG="$JAI_ROOT/vm/jai-test.img"
STAGE="$(mktemp -d /tmp/jai-stage.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

[ -d "$R/boot" ] || { echo "ERROR: no rootfs at $R — run ./build.sh rootfs && sudo ./setup-rootfs.sh first" >&2; exit 1; }
[ -f "$R/boot/vmlinuz" ] || { echo "ERROR: no kernel in rootfs /boot" >&2; exit 1; }

# NOTE: we deliberately do NOT unmount anything from the rootfs.
# Lazy-unmounting bind-mounted /dev subtrees propagated back to the host
# and took down /dev/pts + /dev/shm (2026-10-26 incident, see DECISIONS.md).
# rsync -x below skips mounted virtual filesystems by definition —
# the mount state simply doesn't matter.

echo "[jai] snapshotting rootfs to clean staging dir (rsync -x skips mounts)"
rsync -aHAX --numeric-ids -x "$R/" "$STAGE/"
mkdir -p "$STAGE/dev" "$STAGE/proc" "$STAGE/sys" "$STAGE/run" "$STAGE/tmp"
chmod 1777 "$STAGE/tmp"

echo "[jai] creating 6 GB sparse disk image at $IMG"
rm -f "$IMG"
truncate -s 6G "$IMG"

echo "[jai] formatting ext4 and populating from clean snapshot"
mkfs.ext4 -q -F -L jairoot -E root_owner=0:0 -d "$STAGE" "$IMG"

echo "[jai] DONE — boot with ./boot-vm.sh"
