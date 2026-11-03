#!/usr/bin/env bash
# vm-update.sh - run INSIDE the JAI OS VM: refresh the whole JAI layer from
# repo main, safely. Replaces the old ad-hoc "curl one file into place"
# refreshes (which is how /etc/jai/opencode.json got truncated once).
#
#   curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-update.sh | sudo bash
#
# Rules (learned the hard way, see docs/DECISIONS.md):
#   - download to a temp file FIRST - never pipe into the live file
#   - compile C BEFORE installing - a bad pull must change nothing
#   - back up every replaced file as <name>.bak
#   - validate what can be validated (sudoers parses) before it lands
set -euo pipefail

REPO="${JAI_REPO:-https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
msg() { printf '\033[jai]\033 %s\n' "$*"; }
CHANGED=0

fetch() { # fetch <repo-path> <dest> <mode>
  local src="$1" dst="$2" mode="$3"
  curl -fsSL -o "$TMP/payload" "$REPO/$src"
  [ -s "$TMP/payload" ] || { echo "EMPTY download: $src - aborting, nothing changed"; exit 1; }
  if [ -f "$dst" ] && cmp -s "$TMP/payload" "$dst"; then
    msg "    unchanged: $dst"
    return 0
  fi
  [ -f "$dst" ] && cp "$dst" "$dst.bak"
  install -m "$mode" "$TMP/payload" "$dst"
  msg "    updated:  $dst  (backup: $dst.bak)"
  CHANGED=$((CHANGED + 1))
}

msg "[1/5] toolchain"
apt-get update -qq
apt-get install -y -qq curl gcc libjson-c-dev >/dev/null

msg "[2/5] rebuilding the hands (jai-mcp)"
curl -fsSL -o "$TMP/jai-mcp.c" "$REPO/agent/mcp/jai-mcp.c"
[ -s "$TMP/jai-mcp.c" ] || { echo "EMPTY download: jai-mcp.c - aborting"; exit 1; }
gcc -O2 -Wall -Wextra -o "$TMP/jai-mcp" "$TMP/jai-mcp.c" -ljson-c
if [ -x /usr/bin/jai-mcp ]; then
  OLD="$(/usr/bin/jai-mcp </dev/null 2>&1 | head -1 || true)"
  [ -n "$OLD" ] && msg "    was: $OLD"
  cp /usr/bin/jai-mcp /usr/bin/jai-mcp.bak
fi
install -m 0755 "$TMP/jai-mcp" /usr/bin/jai-mcp
strip /usr/bin/jai-mcp
msg "    installed /usr/bin/jai-mcp ($(/usr/bin/jai-mcp </dev/null 2>&1 | head -1))"

msg "[3/5] rebuilding the reflex arc (jai-eventd)"
curl -fsSL -o "$TMP/jai-eventd.c" "$REPO/agent/eventd/jai-eventd.c"
[ -s "$TMP/jai-eventd.c" ] || { echo "EMPTY download: jai-eventd.c - aborting"; exit 1; }
gcc -O2 -Wall -Wextra -o "$TMP/jai-eventd" "$TMP/jai-eventd.c" -ljson-c
[ -x /usr/bin/jai-eventd ] && cp /usr/bin/jai-eventd /usr/bin/jai-eventd.bak || true
install -m 0755 "$TMP/jai-eventd" /usr/bin/jai-eventd
strip /usr/bin/jai-eventd
msg "    installed /usr/bin/jai-eventd ($(/usr/bin/jai-eventd --version))"

msg "[4/5] units, sudoers, configs (backup-first)"
fetch rootfs/overlay/etc/systemd/system/jai-eventd.service    /etc/systemd/system/jai-eventd.service    0644
fetch rootfs/overlay/etc/systemd/system/jai-heartbeat.service /etc/systemd/system/jai-heartbeat.service 0644
fetch rootfs/overlay/etc/systemd/system/jai-heartbeat.timer   /etc/systemd/system/jai-heartbeat.timer   0644
fetch rootfs/overlay/etc/logrotate.d/jai                      /etc/logrotate.d/jai                      0644
fetch rootfs/overlay/usr/local/bin/jai-heartbeat-beat         /usr/local/bin/jai-heartbeat-beat         0755
fetch rootfs/overlay/usr/local/bin/jai                        /usr/local/bin/jai                        0755
# sudoers: validate BEFORE it lands - a broken sudoers locks everyone out
curl -fsSL -o "$TMP/sudoers" "$REPO/rootfs/overlay/etc/sudoers.d/jai-mcp"
[ -s "$TMP/sudoers" ] || { echo "EMPTY download: sudoers - aborting"; exit 1; }
visudo -cf "$TMP/sudoers" >/dev/null
if [ -f /etc/sudoers.d/jai-mcp ] && cmp -s "$TMP/sudoers" /etc/sudoers.d/jai-mcp; then
  msg "    unchanged: /etc/sudoers.d/jai-mcp"
else
  cp /etc/sudoers.d/jai-mcp /etc/sudoers.d/jai-mcp.bak 2>/dev/null || true
  install -m 0440 "$TMP/sudoers" /etc/sudoers.d/jai-mcp
  msg "    updated:  /etc/sudoers.d/jai-mcp (validated, backup saved)"
  CHANGED=$((CHANGED + 1))
fi
fetch agent/opencode.json /etc/jai/opencode.json 0644
fetch agent/IDENTITY.md   /etc/jai/IDENTITY.md   0644

msg "[5/5] reloading"
mkdir -p /var/lib/jai
chown 1000:1000 /var/lib/jai 2>/dev/null || chown jai:jai /var/lib/jai
chown 1000:1000 /etc/jai/opencode.json /etc/jai/IDENTITY.md 2>/dev/null || true
systemctl daemon-reload
systemctl enable jai-eventd.service >/dev/null 2>&1 || true
systemctl restart jai-eventd.service 2>/dev/null || true
systemctl enable jai-heartbeat.timer >/dev/null 2>&1 || true

echo
if [ "$CHANGED" -gt 0 ]; then
  echo "[jai] vm-update done - $CHANGED file(s) refreshed (.bak saved next to each)"
else
  echo "[jai] vm-update done - everything already current"
fi
echo "[jai] jai-mcp/jai-eventd take effect on the NEXT jai call (stateless-per-call; no restarts needed)."
echo "[jai] verify with: curl -fsSL $REPO/scripts/vm-doctor.sh | bash"
