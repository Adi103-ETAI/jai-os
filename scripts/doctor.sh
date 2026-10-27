#!/usr/bin/env bash
# doctor.sh — JAI OS build host pre-flight check (no root needed)
#
#   ./scripts/doctor.sh
#
# Checks everything the build pipeline needs and reports the current state
# of build artifacts. Exits non-zero only if a CRITICAL check fails.
set -u

JAI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
R="$JAI_ROOT/build/rootfs"

ok()   { printf '  \033[1;32mOK\033[0m       %s\n' "$*"; }
bad()  { printf '  \033[1;31mCRITICAL\033[0m %s\n' "$*"; FAIL=1; }
warn() { printf '  \033[1;33mWARN\033[0m    %s\n' "$*"; }
info() { printf '  \033[1;36mINFO\033[0m    %s\n' "$*"; }

FAIL=0
echo "=== JAI build host doctor ==="
echo "--- toolchain"
for cmd in gcc make git rsync curl; do
  command -v "$cmd" >/dev/null 2>&1 && ok "$cmd" || bad "missing: $cmd"
done
for cmd in debootstrap mkosi ccache qemu-system-x86_64; do
  command -v "$cmd" >/dev/null 2>&1 && ok "$cmd" || warn "missing: $cmd (needed later — ./build.sh deps will flag)"
done

echo "--- kernel build env (for jai_dev module on the HOST)"
KDIR="/lib/modules/$(uname -r)/build"
if [ -d "$KDIR" ]; then
  ok "kernel headers for $(uname -r)"
else
  warn "no headers for $(uname -r) — host-side module builds will fail (chroot builds use scripts/phase2.sh instead)"
fi

echo "--- KVM acceleration"
if [ -e /dev/kvm ]; then
  if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ok "/dev/kvm accessible — VMs will be fast"
  else
    warn "/dev/kvm exists but not accessible (add yourself to the kvm group) — boot-vm.sh falls back to tcg (slow)"
  fi
else
  warn "no /dev/kvm — boot-vm.sh uses tcg emulation (works, slower)"
fi

echo "--- resources"
info "kernel: $(uname -r) | $(nproc) cores | $(free -h | awk '/^Mem:/{print $2}') RAM"
DF="$(df -h "$JAI_ROOT" | awk 'NR==2{print $4}')"
[ "$(df "$JAI_ROOT" | awk 'NR==2{print int($4/1024)}')" -ge 10 ] \
  && ok "disk free: $DF" \
  || bad "disk free: $DF — rootfs + images need ~15 GB"

echo "--- build artifacts state"
if [ -d "$R" ]; then
  [ -x "$R/bin/sh" ] && ok "rootfs exists: $R" || bad "rootfs dir exists but is not a rootfs"
  if [ -e "$R/boot/vmlinuz" ]; then
    ok "VM kernel present: $(basename "$(readlink -f "$R/boot/vmlinuz")")"
  else
    warn "no kernel in rootfs /boot — setup-rootfs.sh not run (or still installing)?"
  fi
  [ -f "$R/usr/local/bin/opencode" ] \
    && ok "opencode baked in (Phase 2 done)" \
    || info "opencode not in rootfs yet — Phase 2 pending (sudo ./scripts/phase2.sh)"
  [ -f "$R/lib/modules"/*/extra/jai_dev.ko ] 2>/dev/null \
    && ok "jai_dev.ko baked into rootfs modules" \
    || info "jai_dev.ko not in rootfs yet — phase2.sh 5c pending"
  # any live chroot mounts right now?
  if grep -qs "$R/\(proc\|sys\|dev\)" /proc/mounts; then
    warn "rootfs has live mounts (safe to keep; make-disk.sh rsync -x ignores them):"
    grep "$R/\(proc\|sys\|dev\)" /proc/mounts | sed 's/^/        /'
  fi
else
  info "no rootfs yet — start with: ./build.sh rootfs && sudo ./setup-rootfs.sh"
fi
[ -f "$JAI_ROOT/vm/jai-test.img" ] && ok "test disk image exists (vm/jai-test.img)" \
  || info "no disk image yet — vm/make-disk.sh pending"

echo "--- repo sanity
"
grep -qE '^systemd-sysv$' "$JAI_ROOT/rootfs/packages.txt" 2>/dev/null \
  && ok "packages.txt pins systemd-sysv (PID 1 guaranteed)" \
  || warn "packages.txt missing systemd-sysv — VM may have no init!"
[ -f "$JAI_ROOT/rootfs/overlay/etc/modules-load.d/jai.conf" ] \
  && ok "modules-load.d/jai.conf overlay present" \
  || warn "modules-load.d overlay missing — module won't load at boot"

echo
if [ "$FAIL" -eq 1 ]; then
  echo "=== doctor: CRITICAL issues above must be fixed before building ==="
  exit 1
fi
echo "=== doctor: no critical issues — you are clear to build ==="
