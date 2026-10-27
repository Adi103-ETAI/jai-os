#!/usr/bin/env bash
# phase2.sh — bake the JAI agent layer into the rootfs (Phase 2 = NEXT-STEPS STEP 5)
#
#   sudo ./scripts/phase2.sh
#
# Automates, in order:
#   5a. opencode binary into the rootfs (/usr/local/bin/opencode)
#   5b. JAI brain config into the rootfs (/etc/jai/, /var/lib/jai)
#   5c. jai_dev kernel module built INSIDE the chroot against the VM's
#       Ubuntu kernel (NOT the host kernel), installed via depmod +
#       modules-load.d so JAI OS loads it at boot
#
# Safety notes (see docs/DECISIONS.md, 2026-10-26 incident):
#   - All rbind mounts are immediately marked --make-rslave, so unmounts
#     can NEVER propagate back to the host.
#   - This script never uses lazy unmounts. Cleanup is umount -R on an
#     rslave tree; if busy, mounts are left in place — make-disk.sh's
#     rsync -x staging is immune to them.
#   - Kernel version is detected from the rootfs's /boot/vmlinuz symlink,
#     never hardcoded.
set -euo pipefail

JAI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
R="$JAI_ROOT/build/rootfs"

msg()  { printf '\033[1;36m[jai]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[jai WARN]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[jai ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- guards ---
[ "$(id -u)" -eq 0 ] || die "run with sudo: sudo ./scripts/phase2.sh"
[ -x "$R/bin/sh" ]   || die "$R is not a rootfs — run ./build.sh rootfs && sudo ./setup-rootfs.sh first"

# ------------------------------------------------- chroot virtual mounts ---
# rbind + IMMEDIATELY rslave — the exact fix adopted after the incident.
mount -t proc  proc  "$R/proc" 2>/dev/null || true
mount --rbind /sys "$R/sys"    2>/dev/null || true
mount --rbind /dev "$R/dev"    2>/dev/null || true
mount --make-rslave "$R/sys" 2>/dev/null || warn "could not mark $R/sys rslave"
mount --make-rslave "$R/dev" 2>/dev/null || warn "could not mark $R/dev rslave"

cleanup() {
  # rslave trees: unmount cannot propagate to the host. Non-lazy by design —
  # if something is busy we LEAVE it mounted (make-disk.sh rsync -x doesn't care).
  umount -R "$R/proc" 2>/dev/null || true
  umount -R "$R/sys"  2>/dev/null || true
  umount -R "$R/dev"  2>/dev/null || true
}
trap cleanup EXIT

# ------------------------------------------------------------ 5a: agent ---
# OpenCode v2 (>= 2.0) is installed DIRECTLY from the npm tarball:
#   - the opencode.ai/install script refuses 2.x ("only installs OpenCode 1")
#   - the v2 installer (opencode.ai/v2/install) has no PREFIX support —
#     it hardcodes $HOME/.opencode/bin, useless for baking into a rootfs
# A pinned, explicit tarball download is the reproducible path for an OS image.
OC_VERSION="${JAI_OPENCODE_VERSION:-2.2.0}"   # override: sudo env JAI_OPENCODE_VERSION=x.y.z ./scripts/phase2.sh
OC_TARBALL_BASE="https://registry.npmjs.org"

if [ -x "$R/usr/local/bin/opencode" ]; then
  msg "5a. opencode already in rootfs — skipping install ($("$R/usr/local/bin/opencode" --version 2>/dev/null || echo 'unknown version'))"
else
  # Same CPU heuristic as the official installer: no AVX2 → x86-64-v3 baseline build
  if grep -qwi avx2 /proc/cpuinfo 2>/dev/null; then
    OC_TARGET="linux-x64"
  else
    OC_TARGET="linux-x64-baseline"
  fi
  msg "5a. installing OpenCode v${OC_VERSION} (${OC_TARGET}) from npm into rootfs"
  TMP_OC="$(mktemp -d)"
  for SCOPE in "@opencode" "@opencode-ai"; do   # @opencode-ai = pre-rename scope, kept as fallback
    OC_URL="$OC_TARBALL_BASE/$SCOPE/cli-$OC_TARGET/-/cli-$OC_TARGET-$OC_VERSION.tgz"
    if curl -fsSL --max-time 120 "$OC_URL" -o "$TMP_OC/cli.tgz"; then
      msg "    $OC_URL"
      break
    fi
    OC_URL=""
  done
  [ -n "$OC_URL" ] || die "OpenCode v$OC_VERSION ($OC_TARGET) not found on npm — set JAI_OPENCODE_VERSION or install manually into $R/usr/local/bin/opencode"
  tar -xzf "$TMP_OC/cli.tgz" -C "$TMP_OC" package/bin/opencode
  install -m 0755 "$TMP_OC/package/bin/opencode" "$R/usr/local/bin/opencode"
  rm -rf "$TMP_OC"
  [ -x "$R/usr/local/bin/opencode" ] || die "opencode install did not produce a usable binary"
  msg "    installed: $("$R/usr/local/bin/opencode" --version 2>/dev/null || ls -lh "$R/usr/local/bin/opencode" | awk '{print $5, $9}')"
fi

# --------------------------------------------------------- 5b: brain cfg ---
msg "5b. installing JAI brain config → /etc/jai"
mkdir -p "$R/etc/jai" "$R/var/lib/jai" "$R/var/lib/jai-memory"
cp "$JAI_ROOT/agent/opencode.json" "$R/etc/jai/opencode.json"
cp "$JAI_ROOT/agent/IDENTITY.md"   "$R/etc/jai/IDENTITY.md"
# jai user is uid 1000 inside the rootfs (host passwd doesn't know it) — use numeric
chown -R 1000:1000 "$R/var/lib/jai" "$R/var/lib/jai-memory"

# --- 5b+: agent↔kernel/OS bridge (jai-system MCP server) ---
msg "5b+. installing jai-system MCP server → /usr/local/jai/mcp"
install -D -m 0755 "$JAI_ROOT/agent/mcp/jai-system.py" "$R/usr/local/jai/mcp/jai-system.py"
# jai_dmesg needs kernel-log access: systemd-journal (journalctl -k) + adm
chroot "$R" usermod -aG adm,systemd-journal jai 2>/dev/null \
  || warn "could not add jai to adm/systemd-journal — jai_dmesg may fail in the VM"

# ----------------------------------------------- 5c: kernel module (VM) ---
# The module must be compiled against the KERNEL INSIDE THE ROOTFS (Ubuntu
# 6.8), not the host's kernel. Two traps handled here:
#   1. uname -r inside the chroot reports the HOST kernel → we must pass
#      KDIR= explicitly to the module Makefile.
#   2. The kernel version changes with apt updates → detect from the
#      /boot/vmlinuz symlink instead of hardcoding.
msg "5c. detecting VM kernel version from rootfs /boot"
KVER="$(basename "$(readlink -f "$R/boot/vmlinuz" 2>/dev/null || true)" | sed 's/^vmlinuz-//')"
if [ -z "$KVER" ] || [ "x$KVER" = "xvmlinuz" ] || [ ! -d "$R/lib/modules/$KVER" ]; then
  KVER="$(ls "$R/lib/modules" 2>/dev/null | grep -E '^[0-9]' | sort -V | tail -1 || true)"
fi
[ -n "${KVER:-}" ] && [ -d "$R/lib/modules/$KVER" ] \
  || die "no kernel modules dir in rootfs — is linux-image-generic installed? (setup-rootfs.sh)"
msg "    target kernel: $KVER"

msg "    apt: linux-headers-$KVER + build-essential inside chroot"
chroot "$R" env DEBIAN_FRONTEND=noninteractive bash -c '
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends "linux-headers-'"$KVER"'" build-essential
  apt-get clean
'

msg "    building jai_dev.ko against $KVER (KDIR override — chroot uname is the host kernel!)"
rm -rf "$R/root/jai_dev"
cp -r "$JAI_ROOT/kernel/jai_dev" "$R/root/jai_dev"
chroot "$R" env KVER="$KVER" bash -c \
  'make -C /root/jai_dev clean >/dev/null 2>&1 || true; make -C /root/jai_dev KDIR="/lib/modules/$KVER/build"'
[ -f "$R/root/jai_dev/jai_dev.ko" ] || die "module build failed inside chroot"

msg "    installing module: /lib/modules/$KVER/extra/jai_dev.ko + depmod"
mkdir -p "$R/lib/modules/$KVER/extra"
cp "$R/root/jai_dev/jai_dev.ko" "$R/lib/modules/$KVER/extra/jai_dev.ko"
rm -rf "$R/root/jai_dev"
chroot "$R" depmod -a "$KVER"

msg "    modules-load.d/jai.conf → module loads at every boot"
mkdir -p "$R/etc/modules-load.d"
printf 'jai_dev\n' > "$R/etc/modules-load.d/jai.conf"

# --------------------------------------------------------- service check ---
if chroot "$R" systemctl is-enabled jai-agent.service >/dev/null 2>&1; then
  msg "jai-agent.service: enabled"
else
  warn "jai-agent.service NOT enabled — re-run sudo ./setup-rootfs.sh"
fi

# ------------------------------------------------------------------ done ---
cat <<EOF

[jai] Phase 2 bake-in COMPLETE — rootfs now contains:
        /usr/local/bin/opencode            (the brain, headless HTTP agent)
        /etc/jai/opencode.json             (persona + permissions + MCP wiring)
        /etc/jai/IDENTITY.md               (who JAI is)
        /usr/local/jai/mcp/jai-system.py   (agent↔kernel/OS bridge: MCP tools)
        /lib/modules/$KVER/extra/jai_dev.ko
        /etc/modules-load.d/jai.conf       (module loads at boot)

Next steps (docs/NEXT-STEPS.md):
  1. sudo chroot $R passwd jai        # VM login password
  2. cd vm && sudo ./make-disk.sh     # pack rootfs → jai-test.img
  3. cd vm && ./boot-vm.sh            # first boot with the agent!
  4. ssh -p 2222 jai@localhost, then:
       systemctl status jai-agent     # → active (running)
       systemctl status jai-link      # → exited (OK) — kernel handshake done
       cat /dev/jai                   # → JAI is awake. Kernel link established.
       dmesg | grep 'jai: event'      # → boot event from the handshake
EOF
