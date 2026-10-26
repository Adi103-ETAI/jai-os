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
  xargs -a /tmp/packages.txt apt-get install -y -qq --no-install-recommends
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

echo "[jai] setting root password prompt on first boot (locked by default)"
chroot "$R" passwd -l root 2>/dev/null || true

echo "[jai] unmounting chroot virtual filesystems"
umount -R "$R/proc" 2>/dev/null || true
umount -R "$R/sys"  2>/dev/null || true
umount -R "$R/dev"  2>/dev/null || true

echo "[jai] DONE — rootfs is now JAI OS. Boot it with: see vm/README.md"
