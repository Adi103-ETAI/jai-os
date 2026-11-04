#!/usr/bin/env bash
# vm-capabilities.sh - run INSIDE the JAI OS VM: give the OS its eyes
# (roadmap 2b - the jai-capabilities hardware probe).
#
# Fetches everything from the repo over the VM's own network (slirp NAT)
# and compiles IN THE VM against the VM's glibc (the phase2 5m lesson:
# build-host != target). For future image bakes, phase2.sh step 5o does
# the same inside the chroot.
#
# One-shot (read it first if you like: scripts/vm-capabilities.sh in the repo):
#
#   curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-capabilities.sh | sudo bash
#
# Steps: toolchain -> fetch source -> compile -> /usr/local/bin ->
#        systemd unit -> enable -> first run -> show the JSON.
set -euo pipefail

REPO="${JAI_REPO:-https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main}"
msg() { printf '\033[1;36m[jai]\033[0m %s\n' "$*"; }

msg "[1/6] toolchain (gcc + json-c)"
apt-get update -qq
apt-get install -y -qq curl gcc libjson-c-dev >/dev/null

msg "[2/6] fetching agent/capabilities/jai-capabilities.c from the repo"
mkdir -p /usr/local/src
curl -fsSL -o /usr/local/src/jai-capabilities.c "$REPO/agent/capabilities/jai-capabilities.c"
wc -l /usr/local/src/jai-capabilities.c

msg "[3/6] compiling + installing /usr/local/bin/jai-capabilities"
gcc -O2 -Wall -Wextra -o /usr/local/bin/jai-capabilities /usr/local/src/jai-capabilities.c -ljson-c
strip /usr/local/bin/jai-capabilities
msg "    profile says: $(/usr/local/bin/jai-capabilities --profile)"

msg "[4/6] systemd unit (oneshot at every boot, Before jai-eventd)"
curl -fsSL -o /etc/systemd/system/jai-capabilities.service \
  "$REPO/rootfs/overlay/etc/systemd/system/jai-capabilities.service"
systemctl daemon-reload
systemctl enable jai-capabilities.service >/dev/null 2>&1 || true

msg "[5/6] first run (writes /etc/jai/capabilities.json, atomic 0644)"
systemctl start jai-capabilities.service
systemctl --no-pager --lines=0 status jai-capabilities.service | head -3 || true

msg "[6/6] the machine, in plain words (jq -r equivalent)"
python3 - <<'PY'
import json
d = json.load(open("/etc/jai/capabilities.json"))
print(f"profile: {d['profile']}  |  accel: {d['accel']}  |  virt: {d['virt']}")
for n in d["notes"]:
    print("  -", n)
PY

cat <<'NOTE'

ROADMAP 2b VM SETUP DONE - the OS has eyes.

acceptance checks (spec: docs/CAPABILITIES-SPEC.md):
  cat /etc/jai/capabilities.json          # valid JSON, schema: 1
  /usr/local/bin/jai-capabilities --profile
  systemd-analyze blame | grep jai-capabilities   # < 1 s budget
  sudo systemctl reboot                   # file regenerates at boot
  curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-doctor.sh | bash
                                          # doctor now checks the probe too
NOTE
