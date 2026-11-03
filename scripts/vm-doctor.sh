#!/usr/bin/env bash
# vm-doctor.sh - run INSIDE the JAI OS VM: one-shot appliance self-test
#
#   curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-doctor.sh | bash
#
# Checks the whole chain: kernel module -> /dev/jai -> eventd capture ->
# hands (jai-mcp) -> telemetry timers -> brain (opencode + credentials) ->
# config sanity -> privileged door. Exit 0 = healthy.
# Run it after every change, before every upgrade, and whenever JAI says
# something weird. (The BUILD-HOST pre-flight is scripts/doctor.sh.)
set -u

ok()   { printf '  \033[32mOK\033[0m       %s\n' "$*"; }
bad()  { printf '  \033[31mCRITICAL\033 %s\n' "$*"; FAIL=1; }
warn() { printf '  \033[33mWARN\033[0m    %s\n' "$*"; }
info() { printf '  \033[36mINFO\033[0m    %s\n' "$*"; }
FAIL=0

echo "=== JAI VM doctor - $(date -u '+%F %T UTC') ==="

echo "--- identity"
info "host $(hostname) | $(uname -r) | $(uptime -p 2>/dev/null || true)"
[ "$(hostname)" = "jai" ] || warn "hostname is not 'jai' - is this the JAI OS VM?"

echo "--- kernel link (jai_dev + /dev/jai)"
if lsmod | grep -q '^jai_dev '; then
  ok "jai_dev module loaded"
else
  bad "jai_dev NOT loaded - check: systemctl status systemd-modules-load; ls /lib/modules/$(uname -r)/extra/"
fi
if [ -e /dev/jai ]; then
  ok "/dev/jai exists ($(ls -l /dev/jai | awk '{print $1, $5}'))"
else
  bad "/dev/jai missing - module loaded but device node absent?"
fi
# a real round-trip: drain the ring (status frame when empty), then write
DRAIN="$(cat /dev/jai 2>/dev/null || true)"
if printf '%s' "$DRAIN" | grep -q 'JAI is awake'; then
  ok "kernel link answers: $(printf '%s' "$DRAIN" | tail -1)"
elif [ -n "$DRAIN" ]; then
  ok "drained pending event(s): $(printf '%s' "$DRAIN" | head -1)"
else
  bad "read on /dev/jai produced nothing"
fi
if echo "doctor ping $(date -u '+%T')" > /dev/jai 2>/dev/null; then
  ok "write to /dev/jai accepted (eventd should capture it below)"
else
  bad "write to /dev/jai failed (permission? mode 0666 expected)"
fi

echo "--- hands (jai-mcp)"
if [ -x /usr/bin/jai-mcp ]; then
  RESP="$(printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
    | timeout 5 /usr/bin/jai-mcp 2>/dev/null | tail -1)"
  case "$RESP" in
    *jai_events*) ok "jai-mcp v1.1.0+ - tools/list answers, jai_events present" ;;
    *'"name"'*)   warn "jai-mcp answers but has no jai_events - old build; run scripts/vm-update.sh" ;;
    *)            bad "jai-mcp did not answer tools/list" ;;
  esac
else
  bad "/usr/bin/jai-mcp missing - Phase 3b not installed here"
fi

echo "--- reflex arc (jai-eventd + events.jsonl)"
if systemctl is-active jai-eventd.service >/dev/null 2>&1; then
  ok "jai-eventd active"
else
  bad "jai-eventd not active - install: scripts/vm-eventd.sh"
fi
systemctl is-enabled jai-eventd.service >/dev/null 2>&1 \
  && ok "jai-eventd enabled (survives reboot)" \
  || warn "jai-eventd not enabled"
sleep 1   # give the drain-poller a tick to capture the doctor ping above
if [ -s /var/lib/jai/events.jsonl ]; then
  ok "events.jsonl live, last record: $(printf '%.95s' "$(tail -1 /var/lib/jai/events.jsonl)")"
else
  warn "events.jsonl missing/empty - fire one: echo hello > /dev/jai, re-run doctor"
fi

echo "--- telemetry (heartbeat)"
if systemctl is-active jai-heartbeat.timer >/dev/null 2>&1; then
  ok "jai-heartbeat.timer active"
else
  bad "jai-heartbeat.timer not active"
fi
[ -f /var/lib/jai/heartbeat.log ] \
  && ok "last beat: $(tail -1 /var/lib/jai/heartbeat.log)" \
  || warn "no heartbeat.log yet"

echo "--- brain (opencode)"
OC="$(command -v opencode || true)"
if [ -n "$OC" ]; then
  ok "opencode at $OC ($("$OC" --version 2>/dev/null | head -1))"
else
  bad "opencode not in PATH"
fi
DB="$(ls "$HOME"/.local/share/opencode/opencode*.db 2>/dev/null | head -1)"
if [ -n "$DB" ]; then
  ok "credential DB present ($(du -h "$DB" | awk '{print $1}'))"
else
  warn "no credential DB - run: opencode auth login"
  warn "  (device flow / console account; service-account keys carry NO free quota)"
fi
if [ -f /etc/jai/opencode.json ]; then
  if grep -q '"autoupdate": *false' /etc/jai/opencode.json; then
    ok "brain pinned (autoupdate=false)"
  else
    warn "autoupdate not pinned - add \"autoupdate\": false (unattended self-updates = unattended breakage)"
  fi
  grep -o '"model": *"[^"]*"' /etc/jai/opencode.json | head -1 | sed 's/^/          /'
else
  bad "/etc/jai/opencode.json missing"
fi

echo "--- privileged door (sudoers)"
if [ -f /etc/sudoers.d/jai-mcp ]; then
  M="$(stat -c %a /etc/sudoers.d/jai-mcp)"
  if [ "$M" = "440" ]; then
    ok "sudoers fragment present, mode 440"
  else
    warn "sudoers fragment mode is $M (expected 440)"
  fi
else
  warn "/etc/sudoers.d/jai-mcp missing - service_restart will fail"
fi

echo "--- resources"
info "disk / free: $(df -h / | awk 'NR==2{print $4}') | memory available: $(free -h | awk '/^Mem:/{print $7}')"

echo
if [ "$FAIL" -eq 1 ]; then
  echo "=== doctor: CRITICAL issues above ==="
  exit 1
fi
echo "=== doctor: appliance healthy ==="
