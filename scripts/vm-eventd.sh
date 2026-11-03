#!/usr/bin/env bash
# vm-eventd.sh - run INSIDE the JAI OS VM: attach the reflex arc (jai-eventd)
#
# Fetches everything from the repo over the VM's own network (slirp NAT),
# exactly like vm-jai-mcp.sh does for the hands. One-shot:
#
#   curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-eventd.sh | sudo bash
#
# Steps: toolchain -> fetch source -> compile -> /usr/bin/jai-eventd ->
#        unit + logrotate -> heartbeat beat script + fixed unit ->
#        enable + fire a test event -> show the captured record.
set -euo pipefail

REPO="${JAI_REPO:-https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
msg() { printf '\033[jai]\033 %s\n' "$*"; }

msg "[1/6] toolchain (gcc; libjson-c5 runtime is already pinned by packages.txt)"
apt-get update -qq
apt-get install -y -qq curl gcc libjson-c-dev >/dev/null

msg "[2/6] fetching agent/eventd/jai-eventd.c from the repo"
curl -fsSL -o "$TMP/jai-eventd.c" "$REPO/agent/eventd/jai-eventd.c"
[ -s "$TMP/jai-eventd.c" ] || { echo "EMPTY download - aborting, nothing changed"; exit 1; }
wc -l "$TMP/jai-eventd.c"

msg "[3/6] compiling + installing /usr/bin/jai-eventd (compile BEFORE install - a bad pull changes nothing)"
gcc -O2 -Wall -Wextra -o "$TMP/jai-eventd" "$TMP/jai-eventd.c" -ljson-c
[ -x /usr/bin/jai-eventd ] && cp /usr/bin/jai-eventd /usr/bin/jai-eventd.bak || true
install -m 0755 "$TMP/jai-eventd" /usr/bin/jai-eventd
strip /usr/bin/jai-eventd
msg "    installed: $(/usr/bin/jai-eventd --version)"

msg "[4/6] unit + logrotate + heartbeat beat script (backup-first)"
for f in \
  "rootfs/overlay/etc/systemd/system/jai-eventd.service:/etc/systemd/system/jai-eventd.service:0644" \
  "rootfs/overlay/etc/logrotate.d/jai:/etc/logrotate.d/jai:0644" \
  "rootfs/overlay/usr/local/bin/jai-heartbeat-beat:/usr/local/bin/jai-heartbeat-beat:0755" \
  "rootfs/overlay/etc/systemd/system/jai-heartbeat.service:/etc/systemd/system/jai-heartbeat.service:0644" \
  "rootfs/overlay/etc/systemd/system/jai-heartbeat.timer:/etc/systemd/system/jai-heartbeat.timer:0644" \
; do
  src="${f%%:*}"; rest="${f#*:}"; dst="${rest%%:*}"; mode="${rest##*:}"
  curl -fsSL -o "$TMP/payload" "$REPO/$src"
  [ -s "$TMP/payload" ] || { echo "EMPTY download: $src - aborting"; exit 1; }
  [ -f "$dst" ] && cp "$dst" "$dst.bak"
  install -m "$mode" "$TMP/payload" "$dst"
  msg "    installed $dst (backup: $dst.bak)"
done

msg "[5/6] permissions + enable"
mkdir -p /var/lib/jai
chown 1000:1000 /var/lib/jai 2>/dev/null || chown jai:jai /var/lib/jai
systemctl daemon-reload
systemctl enable jai-eventd.service >/dev/null 2>&1 || true
systemctl restart jai-eventd.service
systemctl enable jai-heartbeat.timer >/dev/null 2>&1 || true
systemctl start  jai-heartbeat.timer

msg "[6/6] firing a test event at the kernel"
echo "eventd online test $(date -u '+%T')" > /dev/jai || true
sleep 1
systemctl --no-pager --lines=3 status jai-eventd.service || true
echo
tail -n 3 /var/lib/jai/events.jsonl 2>/dev/null \
  || echo "(no events captured yet - check: systemctl status jai-eventd)"

cat <<'NOTE'

PHASE 3C VM SETUP DONE - the reflex arc is attached.

1. pipeline check by hand:
   echo "hello kernel" > /dev/jai && sleep 1 && tail -n 2 /var/lib/jai/events.jsonl
2. give the brain the new tool (jai_events lives in jai-mcp v1.1.0):
   curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-update.sh | sudo bash
3. ask it:   jai "Use jai_events to report the last 3 kernel events."
4. self-test anytime:
   curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-doctor.sh | bash
NOTE
