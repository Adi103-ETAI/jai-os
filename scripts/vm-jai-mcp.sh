#!/usr/bin/env bash
# vm-jai-mcp.sh - run INSIDE the JAI OS VM: attach the C hands (jai-mcp)
#
# Fetches everything from the repo over the VM's own network (slirp NAT),
# so nobody has to paste a 600-line C file into a serial console again.
#
# One-shot (read it first if you like: scripts/vm-jai-mcp.sh in the repo):
#
#   curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-jai-mcp.sh | sudo bash
#
# Steps: toolchain -> fetch source -> compile -> /usr/bin/jai-mcp ->
#        sudoers allowlist -> heartbeat test rig -> journal/memory groups ->
#        agent config (backup first).
set -euo pipefail

REPO="${JAI_REPO:-https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main}"
msg() { printf '\033[1;36m[jai]\033[0m %s\n' "$*"; }

msg "[1/7] toolchain (gcc + json-c)"
apt-get update -qq
apt-get install -y -qq curl gcc libjson-c-dev >/dev/null

msg "[2/7] fetching agent/mcp/jai-mcp.c from the repo"
mkdir -p /usr/local/src
curl -fsSL -o /usr/local/src/jai-mcp.c "$REPO/agent/mcp/jai-mcp.c"
wc -l /usr/local/src/jai-mcp.c

msg "[3/7] compiling + installing /usr/bin/jai-mcp"
gcc -O2 -Wall -Wextra -o /usr/bin/jai-mcp /usr/local/src/jai-mcp.c -ljson-c
strip /usr/bin/jai-mcp
/usr/bin/jai-mcp < /dev/null || true   # banner check: server starts and exits on EOF

msg "[4/7] sudoers allowlist (the only privileged door)"
curl -fsSL -o /etc/sudoers.d/jai-mcp "$REPO/rootfs/overlay/etc/sudoers.d/jai-mcp"
chmod 440 /etc/sudoers.d/jai-mcp
visudo -cf /etc/sudoers.d/jai-mcp

msg "[5/7] heartbeat test rig (safe restart target for service_status/restart)"
curl -fsSL -o /etc/systemd/system/jai-heartbeat.service "$REPO/rootfs/overlay/etc/systemd/system/jai-heartbeat.service"
curl -fsSL -o /etc/systemd/system/jai-heartbeat.timer   "$REPO/rootfs/overlay/etc/systemd/system/jai-heartbeat.timer"
systemctl daemon-reload
systemctl enable jai-heartbeat.timer >/dev/null 2>&1 || true
systemctl start  jai-heartbeat.timer
systemctl start  jai-heartbeat.service || true

msg "[6/7] journal + memory permissions for user jai"
usermod -aG adm,systemd-journal jai
mkdir -p /var/lib/jai-memory
chown 1000:1000 /var/lib/jai-memory

msg "[7/7] agent config (backup first: /etc/jai/opencode.json.bak)"
cp /etc/jai/opencode.json /etc/jai/opencode.json.bak 2>/dev/null || true
curl -fsSL -o /etc/jai/opencode.json "$REPO/agent/opencode.json"
curl -fsSL -o /etc/jai/IDENTITY.md   "$REPO/agent/IDENTITY.md"

cat <<'NOTE'

PHASE 3b VM SETUP DONE - the hands are attached.

1. log out and back in (the journal group only applies to new sessions)
2. protocol smoke test (no AI involved):
   printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}' '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jai_sysinfo","arguments":{}}}' | /usr/bin/jai-mcp
3. give the brain hands:
   OPENCODE_CONFIG=/etc/jai/opencode.json opencode run --standalone \
     "Use your jai-mcp tools: report jai_sysinfo, disk_usage on /, and whether jai-heartbeat.service is healthy."
NOTE
