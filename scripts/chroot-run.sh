#!/usr/bin/env bash
# chroot-run.sh — run a command inside build/rootfs with /proc, /sys, /dev mounted
#
#   sudo ./scripts/chroot-run.sh                                # interactive shell
#   sudo ./scripts/chroot-run.sh /usr/local/bin/opencode --version
#   sudo ./scripts/chroot-run.sh ls -la /etc/jai
#
# WHY THIS EXISTS (2026-10-27 incident, docs/DECISIONS.md):
# A debootstrap rootfs has NO /proc until you mount it. Bun standalone
# executables — which opencode v2 is — locate their embedded payload via
# /proc/self/exe. In a bare chroot they die ~1 ms after start with:
#     panic(main thread): abort() called
#     oh no: Bun has crashed. This indicates a bug in Bun, not your code.
# That is an environment artifact, not a broken binary. systemd mounts
# /proc,/sys,/dev for real at VM boot, so "works here" ≈ "works in the VM".
#
# Mount hygiene (2026-10-26 incident): rbind subtrees are marked
# --make-rslave IMMEDIATELY, so unmounts can never propagate to the host.
# No lazy unmounts — if busy, mounts are left in place; make-disk.sh's
# rsync -x staging is immune to them.
set -euo pipefail

JAI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
R="$JAI_ROOT/build/rootfs"

msg() { printf '\033[1;36m[jai]\033[0m %s\n' "$*"; }

[ "$(id -u)" -eq 0 ] || { echo "ERROR: run with sudo: sudo ./scripts/chroot-run.sh [cmd...]" >&2; exit 1; }
[ -x "$R/bin/sh" ]   || { echo "ERROR: $R is not a rootfs — run ./build.sh rootfs && sudo ./setup-rootfs.sh first" >&2; exit 1; }

# Mount only what is missing (idempotent — safe even next to phase2 leftovers)
WE_PROC=0; WE_SYS=0; WE_DEV=0
if ! mountpoint -q "$R/proc"; then mount -t proc  proc  "$R/proc"; WE_PROC=1; fi
if ! mountpoint -q "$R/sys"; then
  mount --rbind /sys "$R/sys"; mount --make-rslave "$R/sys"; WE_SYS=1
fi
if ! mountpoint -q "$R/dev"; then
  mount --rbind /dev "$R/dev"; mount --make-rslave "$R/dev"; WE_DEV=1
fi

cleanup() {
  # rslave trees: an unmount here can never propagate back to the host
  if [ "$WE_DEV"  -eq 1 ]; then umount -R "$R/dev"  2>/dev/null || true; fi
  if [ "$WE_SYS"  -eq 1 ]; then umount -R "$R/sys"  2>/dev/null || true; fi
  if [ "$WE_PROC" -eq 1 ]; then umount -R "$R/proc" 2>/dev/null || true; fi
}
trap cleanup EXIT

if [ "$#" -eq 0 ]; then
  msg "shell in $R — /proc,/sys,/dev mounted. Type 'exit' to leave."
  # shellcheck disable=SC2086
  chroot "$R" env \
    TERM="${TERM:-xterm-256color}" \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    PS1='\[\e[1;36m\]jai-chroot\[\e[0m\]:\w\$ ' \
    /bin/bash --norc -i
else
  chroot "$R" "$@"
fi
