#!/usr/bin/env bash
# provision.sh - ONE-COMMAND rebuild: bare rootfs -> JAI OS -> bootable disk
#
#   sudo ./provision.sh            # full pipeline (skips finished stages)
#   sudo ./provision.sh --fresh    # wipe build/ first (the base is disposable)
#
# Chains the existing pipeline with guards:
#   doctor -> build.sh rootfs -> setup-rootfs.sh -> scripts/phase2.sh -> make-disk.sh
#
# Every stage is idempotent-ish: build.sh rootfs skips debootstrap when the
# rootfs exists, phase2.sh skips opencode when already baked. A --fresh run
# is the true from-scratch path.
#
# What stays MANUAL on purpose (interactive or secret):
#   - sudo chroot build/rootfs passwd jai    (VM login password)
#   - opencode auth login inside the VM      (device flow; credentials never
#     live in the repo or the image)
set -euo pipefail
JAI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$JAI_ROOT"
msg() { printf '\033[jai]\033 %s\n' "$*"; }

[ "$(id -u)" -eq 0 ] || { echo "run with sudo: sudo ./provision.sh" >&2; exit 1; }

if pgrep -f "jai-test.img" >/dev/null 2>&1; then
  echo "ERROR: a JAI VM is currently running - stop it first:" >&2
  echo "  pkill -f jai-test.img      (or Ctrl+A then X in its serial console)" >&2
  exit 1
fi

if [ "${1:-}" = "--fresh" ]; then
  msg "--fresh: wiping build/ (the Ubuntu base is disposable by design)"
  rm -rf build
fi

msg "[1/5] pre-flight (scripts/doctor.sh)"
./scripts/doctor.sh

msg "[2/5] base rootfs (build.sh rootfs)"
./build.sh rootfs

msg "[3/5] JAI OS transformation (setup-rootfs.sh)"
./setup-rootfs.sh

msg "[4/5] agent layer bake-in (scripts/phase2.sh: opencode, module, jai-mcp, jai-eventd)"
./scripts/phase2.sh

msg "[5/5] disk image (vm/make-disk.sh)"
( cd vm && ./make-disk.sh )

cat <<'EOF'

[jai] PROVISIONING COMPLETE - vm/jai-test.img is ready.

  Manual steps that stay manual (interactive or secret):
    1. sudo chroot build/rootfs passwd jai     # VM login password
    2. (cd vm && ./boot-vm.sh)                 # tmux window 1
    3. ssh -p 2222 jai@localhost               # tmux window 2
    4. opencode auth login                     # IN THE VM, device flow
       (open the printed URL in the host browser; service-account keys
        carry NO free quota - use the console account)
    5. in-VM self-test:
       curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-doctor.sh | bash

  Reminder: a --fresh rebuild starts from a clean rootfs - the old VM's
  credential DB does not carry over. Step 4 is ~1 minute. Full runbook:
  docs/NEXT-STEPS.md
EOF
