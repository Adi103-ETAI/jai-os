#!/usr/bin/env bash
# boot-vm.sh — boot JAI OS in QEMU, serial console (no root needed)
# Login over serial or:  ssh -p 2222 jai@localhost   (password set via chroot)
set -euo pipefail

JAI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
R="$JAI_ROOT/build/rootfs"
IMG="$JAI_ROOT/vm/jai-test.img"

[ -f "$IMG" ] || { echo "ERROR: no disk image — run sudo ./make-disk.sh first" >&2; exit 1; }
K="$R/boot/vmlinuz"
I="$R/boot/initrd.img"
[ -e "$K" ] && [ -e "$I" ] || { echo "ERROR: kernel/initrd missing in $R/boot" >&2; exit 1; }

exec qemu-system-x86_64 \
  -accel tcg \
  -m 1024 -smp 1 \
  -kernel "$K" -initrd "$I" \
  -append "root=/dev/vda rw console=ttyS0 loglevel=4" \
  -drive file="$IMG",format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 \
  -device e1000e,netdev=n0 \
  -device virtio-rng-pci \
  -nographic
