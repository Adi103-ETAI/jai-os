#!/usr/bin/env bash
# vm-upgrade-brain.sh - deliberate, backed-up opencode upgrades for JAI OS
#
#   bash vm-upgrade-brain.sh            # upgrade to latest via `opencode upgrade`
#   bash vm-upgrade-brain.sh 2.2.5      # upgrade to a pinned npm tarball version
#
# The appliance pins its brain ("autoupdate": false) on purpose - config
# schema drift has bitten before (v1 -> v2). This makes the bump a
# controlled act: snapshot the credential DB, upgrade, smoke-test, and
# print the rollback path. No restarts needed afterwards: opencode is
# stateless-per-call, the next `jai` invocation runs the new binary.
#
# Run INSIDE the VM, as the jai user (sudo only needed for pinned installs).
set -euo pipefail
TARGET="${1:-latest}"
msg() { printf '\033[jai]\033 %s\n' "$*"; }

command -v opencode >/dev/null || { echo "opencode not in PATH" >&2; exit 1; }
CUR="$(opencode --version 2>/dev/null || echo unknown)"
msg "current brain: $CUR"

# ---- 1. snapshot the credential DB (schema migrations are one-way) -------
STAMP="$(date -u '+%Y%m%dT%H%M%SZ')"
BK="$HOME/opencode-db-backup-$STAMP"
if ls "$HOME"/.local/share/opencode/opencode*.db* >/dev/null 2>&1; then
  mkdir -p "$BK"
  cp -a "$HOME"/.local/share/opencode/opencode*.db* "$BK"/
  msg "credential DB backed up -> $BK"
else
  msg "no credential DB found (opencode auth login never ran here?)"
fi

# ---- 2. upgrade -----------------------------------------------------------
if [ "$TARGET" = "latest" ]; then
  msg "running: opencode upgrade"
  if ! opencode upgrade; then
    echo "  'opencode upgrade' failed or is unsupported in this build." >&2
    echo "  Use the pinned path instead:  bash vm-upgrade-brain.sh <version>" >&2
    exit 1
  fi
else
  msg "installing pinned OpenCode v$TARGET (npm tarball, same path as phase2.sh 5a)"
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  DOWNLOADED=""
  for SCOPE in "@opencode" "@opencode-ai"; do   # @opencode-ai = pre-rename scope
    URL="https://registry.npmjs.org/$SCOPE/cli-linux-x64-baseline/-/cli-linux-x64-baseline-$TARGET.tgz"
    if curl -fsSL --max-time 120 "$URL" -o "$TMP/cli.tgz"; then
      DOWNLOADED="$URL"; break
    fi
  done
  [ -n "$DOWNLOADED" ] || { echo "v$TARGET not found on npm" >&2; exit 1; }
  msg "    $DOWNLOADED"
  tar -xzf "$TMP/cli.tgz" -C "$TMP" package/bin/opencode
  sudo install -m 0755 "$TMP/package/bin/opencode" /usr/local/bin/opencode
fi

NEW="$(opencode --version 2>/dev/null || echo BROKEN)"
msg "brain is now: $NEW"

# ---- 3. smoke test --------------------------------------------------------
msg "smoke test: one round-trip through the brain"
SMOKE_OK=0
if command -v jai >/dev/null 2>&1; then
  if jai "One-shot reply, no tools: say exactly 'brain online'." 2>&1 | tee /dev/stderr | grep -qi 'brain online'; then
    SMOKE_OK=1
  fi
else
  if OPENCODE_CONFIG=/etc/jai/opencode.json opencode run --standalone \
      "One-shot reply, no tools: say exactly 'brain online'." 2>&1 | grep -qi 'brain online'; then
    SMOKE_OK=1
  fi
fi

if [ "$SMOKE_OK" -eq 1 ]; then
  msg "upgrade COMPLETE - $CUR -> $NEW (db backup: $BK)"
else
  cat >&2 <<EOF

  SMOKE TEST FAILED after the upgrade. Roll back:

    # 1. restore the old binary (pick one):
    sudo ./scripts/phase2.sh   # re-bakes with JAI_OPENCODE_VERSION=${CUR:-2.2.0}
    #    or manually: download the old npm tarball (see this script's pinned
    #    path) and: sudo install -m 0755 <old-opencode> /usr/local/bin/opencode

    # 2. only if the NEW version migrated the DB and the old binary chokes:
    rm ~/.local/share/opencode/opencode*.db*    # then re-run: opencode auth login
    #    or restore the snapshot: cp -a $BK/* ~/.local/share/opencode/

  db backup is at: $BK
EOF
  exit 1
fi
