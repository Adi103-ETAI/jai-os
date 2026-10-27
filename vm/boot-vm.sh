#!/usr/bin/env bash
# boot-vm.sh — boot JAI OS in QEMU, serial console (no root needed)
# Login over serial or:  ssh -p 2222 jai@localhost   (password set via chroot)
set -euo pipefail

JAI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
R="$JAI_ROOT/build/rootfs"
IMG="$JAI_ROOT/vm/jai-test.img"

[ -f "$IMG" ] || { echo "ERROR: no disk image — run sudo ./make-disk.sh first" >&2; exit 1; }
[ -r "$IMG" ] && [ -w "$IMG" ] || {
  echo "ERROR: $IMG is not accessible by $(id -un) — it was created by root." >&2
  echo "  fix:  sudo chown $(id -un):$(id -un) $IMG" >&2
  exit 1
}
K="$R/boot/vmlinuz"
I="$R/boot/initrd.img"
[ -e "$K" ] && [ -e "$I" ] || { echo "ERROR: kernel/initrd missing in $R/boot" >&2; exit 1; }

# Guard 1: is the previous JAI VM still alive? (it holds TCP 2222 via hostfwd)
if pgrep -f "jai-test.img" >/dev/null 2>&1; then
  echo "ERROR: the previous JAI VM is still running (holding TCP 2222)." >&2
  echo "  stop it:  pkill -f jai-test.img     (or Ctrl+A then X in its serial console)" >&2
  exit 1
fi
# Guard 2: something ELSE squatting on the SSH forward port
if command -v ss >/dev/null 2>&1 && ss -ltnH 2>/dev/null | grep -q ':2222 '; then
  echo "ERROR: TCP 2222 is already in use by another process (needed for ssh -p 2222)." >&2
  echo "  find it:  ss -ltnp 'sport = :2222'" >&2
  exit 1
fi

# KVM when permitted, tcg otherwise (warn so nobody wonders why boot is slow)
ACCEL=tcg
if [ -w /dev/kvm ]; then
  ACCEL=kvm
else
  echo "[jai] /dev/kvm not writable — using tcg emulation (slow)." >&2
  echo "[jai] fix:  sudo usermod -aG kvm $(id -un) && newgrp kvm" >&2
  if id -nG | grep -qw kvm; then
    echo "[jai] NOTE: you ARE in the kvm group, but /dev/kvm still denies you." >&2
    echo "[jai] check:  ls -l /dev/kvm   (should be root:kvm, mode 660+)" >&2
    echo "[jai] quick fix until reboot:  sudo chmod 666 /dev/kvm" >&2
  fi
fi

# CPU model follows the accelerator: host passthrough under KVM; under TCG
# use max (all features TCG can emulate) instead of the feature-poor qemu64
# default — a guest built with AVX2 SIGILLs on qemu64.
if [ "$ACCEL" = kvm ]; then
  CPU_MODEL="host"
else
  CPU_MODEL="max"
fi

exec qemu-system-x86_64 \
  -accel "$ACCEL" \
  -cpu "$CPU_MODEL" \
  -m 1024 -smp 1 \
  -kernel "$K" -initrd "$I" \
  -append "root=/dev/vda rw console=ttyS0 loglevel=4" \
  -drive file="$IMG",format=raw,if=virtio \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 \
  -device e1000e,netdev=n0 \
  -device virtio-rng-pci \
  -nographic
