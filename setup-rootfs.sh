#!/usr/bin/env bash
# setup-rootfs.sh — turn a bare debootstrap rootfs into JAI OS
# Run AFTER ./build.sh rootfs finishes. Needs sudo (run from your tmux session):
#   sudo ./setup-rootfs.sh
set -euo pipefail

JAI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$JAI_ROOT/build/rootfs"
RELEASE=noble

[ -x "$R/bin/sh" ] || { echo "ERROR: $R does not look like a rootfs — run ./build.sh rootfs first" >&2; exit 1; }

echo "[jai] mounting virtual filesystems into chroot"
mount -t proc  proc  "$R/proc"     2>/dev/null || true
mount --rbind /sys   "$R/sys"      2>/dev/null || true
mount --rbind /dev   "$R/dev"      2>/dev/null || true
# CRITICAL: mark the bind trees rslave so unmounts never propagate back
# to the host (2026-10-26 incident: lazy unmount killed host /dev/pts)
mount --make-rslave "$R/sys" 2>/dev/null || true
mount --make-rslave "$R/dev" 2>/dev/null || true
mkdir -p "$R/run/systemd" 2>/dev/null || true

echo "[jai] writing apt sources (Ubuntu $RELEASE)"
cat > "$R/etc/apt/sources.list" <<EOF
deb http://archive.ubuntu.com/ubuntu $RELEASE main universe
deb http://archive.ubuntu.com/ubuntu $RELEASE-updates main universe
deb http://security.ubuntu.com/ubuntu $RELEASE-security main universe
EOF

cat > "$R/etc/apt/apt.conf.d/90no-recommends" <<'EOF'
APT::Install-Recommends "false";
EOF

echo "[jai] hostname: jai"
echo "jai" > "$R/etc/hostname"
grep -q jai "$R/etc/hosts" 2>/dev/null || \
  echo "127.0.1.1  jai" >> "$R/etc/hosts"

echo "[jai] copying package list into chroot (chroot can't see host paths)"
cp "$JAI_ROOT/rootfs/packages.txt" "$R/tmp/packages.txt"

echo "[jai] installing JAI package set (this takes a while)"
chroot "$R" env DEBIAN_FRONTEND=noninteractive bash -c '
  apt-get update -qq
  grep -vE "^\s*(#|$)" /tmp/packages.txt |
    xargs apt-get install -y -qq --no-install-recommends
  rm -f /tmp/packages.txt
  apt-get clean
'

echo "[jai] creating jai user (uid 1000, sudo, no password enforced yet)"
chroot "$R" bash -c '
  id -u jai >/dev/null 2>&1 || useradd -m -s /bin/bash -u 1000 jai
  usermod -aG sudo jai
  mkdir -p /var/lib/jai-memory && chown jai:jai /var/lib/jai-memory
'

echo "[jai] enabling JAI services"
if chroot "$R" systemctl enable jai-agent.service 2>/dev/null; then
  echo "[jai]   jai-agent enabled via systemctl"
else
  echo "[jai]   systemctl unavailable — boot symlink already in place (verified)"
fi

echo "[jai] configuring VM networking (2026-10-27: minbase has no network stack)"
# systemd-networkd ships IN the systemd package — it only needs a .network
# file + enablement. Overlay copy came from build.sh; assert both here so a
# pre-overlay rootfs also gets fixed by a re-run:
mkdir -p "$R/etc/systemd/network"
cp "$JAI_ROOT/rootfs/overlay/etc/systemd/network/20-jai.network" \
   "$R/etc/systemd/network/20-jai.network" 2>/dev/null || true
if chroot "$R" systemctl enable systemd-networkd.service 2>/dev/null; then
  echo "[jai]   systemd-networkd enabled"
else
  # offline fallback: plain symlink = exactly what `systemctl enable` does
  mkdir -p "$R/etc/systemd/system/multi-user.target.wants"
  ln -sf /usr/lib/systemd/system/systemd-networkd.service \
         "$R/etc/systemd/system/multi-user.target.wants/systemd-networkd.service"
  [ -x "$R/usr/lib/systemd/systemd-networkd" ] || [ -x "$R/lib/systemd/systemd-networkd" ] \
    || echo "[jai]   WARN: systemd-networkd binary not found — check the systemd package"
  echo "[jai]   systemd-networkd enabled via symlink"
fi
# Static DNS for the VM: minbase has no systemd-resolved (glibc reads this
# file directly) and slirp's 10.0.2.3 forwarder is unreliable (EAI_AGAIN).
# Public resolvers go straight through slirp NAT. Override on the host:
#   sudo env JAI_VM_DNS="9.9.9.9 149.112.112.112" ./setup-rootfs.sh
JAI_VM_DNS="${JAI_VM_DNS:-1.1.1.1 8.8.8.8 9.9.9.9}"
{
  echo "# JAI OS appliance DNS — static by design (see overlay 20-jai.network)"
  for ns in $JAI_VM_DNS; do echo "nameserver $ns"; done
} > "$R/etc/resolv.conf"
echo "[jai]   VM resolv.conf → $(echo $JAI_VM_DNS)"

echo "[jai] setting root password prompt on first boot (locked by default)"
chroot "$R" passwd -l root 2>/dev/null || true

echo "[jai] unmounting chroot virtual filesystems (rslave — cannot propagate)"
umount -R "$R/proc" 2>/dev/null || true
umount -R "$R/sys"  2>/dev/null || true
umount -R "$R/dev"  2>/dev/null || true

echo "[jai] DONE — rootfs is now JAI OS. Boot it with: see vm/README.md"
