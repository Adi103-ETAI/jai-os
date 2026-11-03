# JAI — Next Steps

Do these in order. Each step lists exact commands. Where `sudo` appears,
run it in your tmux session (`tmux attach -t jai`).

## STEP 0 — Pre-flight check
```bash
cd ~/jai-os && ./scripts/doctor.sh
```
Reports toolchain, headers, KVM, disk, and current artifact state.
Fix anything CRITICAL before continuing.

## STEP 1 — Pack the disk image (Phase 1 finish)
```bash
cd ~/jai-os/vm
sudo ./make-disk.sh        # rsync -x staging — immune to any chroot mounts
```
Expected: "DONE — boot with ./boot-vm.sh", ~30 seconds.

## STEP 2 — Set the jai user's password (for VM login)
```bash
cd ~/jai-os
sudo chroot build/rootfs passwd jai
```
Remember this password — you'll use it to SSH into the VM.
TIP (2026-10-27): a bare chroot like this is fine for `passwd`, but NOT
for running Bun-based binaries — opencode aborts there without /proc.
To run or inspect things in the rootfs use:
```bash
sudo ./scripts/chroot-run.sh                            # interactive shell
sudo ./scripts/chroot-run.sh /usr/local/bin/opencode --version
```

## STEP 3 — Boot JAI OS (first boot!)
In tmux:
```bash
cd ~/jai-os/vm && ./boot-vm.sh
```
- Serial console scrolls the Linux 6.8 boot.
- **Known, harmless:** `jai-agent.service` fails/retries — `opencode`
  isn't installed inside the rootfs yet (that's Step 5).
- Ends at a login prompt on host `jai`.

From a second terminal:
```bash
ssh -p 2222 jai@localhost
```
Then inside the VM, verify:
```bash
hostname          # → jai
uname -r          # → 6.8.0-147-generic
systemctl status jai-agent   # → failed (expected, see Step 5)
free -h           # ~1 GB VM
```
Exit QEMU with Ctrl+A then X.

## STEP 4 — Commit + push the milestone
```bash
cd ~/jai-os && git add -A && git commit -m "Phase 1 complete: JAI OS boots in test VM"
git push
```
(If push asks for auth, generate a fresh token on GitHub.)

## STEP 5 — Bake the agent layer (Phase 2) — ONE COMMAND
```bash
cd ~/jai-os && sudo ./scripts/phase2.sh
```
This automates everything below (opencode install, /etc/jai config,
module rebuild inside the chroot, depmod, modules-load.d) with the
incident lessons applied: rbind mounts are immediately `--make-rslave`,
kernel version is detected from the rootfs (never hardcoded), and the
module Makefile gets an explicit `KDIR=` (inside a chroot, `uname -r`
reports the HOST kernel — hardcoding or trusting it silently builds
against the wrong kernel).
It also STAMPS the baked opencode build to `/etc/jai/opencode-build`
(version/target/date) and smoke-tests `opencode --version` inside the
chroot — a bad bake now fails the build immediately instead of
crash-looping `jai-agent` at boot.

<details><summary>Manual equivalent (legacy — kept for reference)</summary>

### 5a. opencode into the rootfs
NOTE: the rootfs is root-owned (created by sudo debootstrap), so the
installer must run as root:
```bash
cd ~/jai-os
curl -fsSL https://opencode.ai/install | \
  sudo env PREFIX=build/rootfs/usr/local bash
```
(If the installer misbehaves, download the release binary manually and
place it at `build/rootfs/usr/local/bin/opencode`.)

### 5b. JAI's brain config
```bash
sudo mkdir -p build/rootfs/etc/jai build/rootfs/var/lib/jai
sudo cp agent/opencode.json  build/rootfs/etc/jai/
sudo cp agent/IDENTITY.md    build/rootfs/etc/jai/
sudo chown -R 1000:1000 build/rootfs/var/lib/jai
```

### 5c. Kernel module for the VM's Ubuntu kernel
The module we built targets the HOST kernel. Rebuild it inside the chroot.
CRITICAL (2026-10-26 incident): rbind mounts MUST be marked rslave
immediately, or unmounts can propagate and kill the host's /dev/pts +
cgroupfs. Also pass KDIR= explicitly — chroot `uname -r` is the host kernel.
```bash
sudo mount -t proc proc build/rootfs/proc
sudo mount --rbind /sys build/rootfs/sys
sudo mount --rbind /dev build/rootfs/dev
sudo mount --make-rslave build/rootfs/sys
sudo mount --make-rslave build/rootfs/dev
KVER=$(basename "$(readlink -f build/rootfs/boot/vmlinuz)" | sed 's/^vmlinuz-//')
sudo chroot build/rootfs bash -c "
  apt-get update && apt-get install -y linux-headers-$KVER build-essential
"
sudo cp -r kernel/jai_dev build/rootfs/root/
sudo chroot build/rootfs make -C /root/jai_dev KDIR=/lib/modules/$KVER/build
sudo chroot build/rootfs bash -c "
  echo jai_dev > /etc/modules-load.d/jai.conf
  cp /root/jai_dev/jai_dev.ko /lib/modules/$KVER/extra/
  depmod -a $KVER
"
sudo umount -R build/rootfs/dev build/rootfs/sys build/rootfs/proc
```

</details>

### 5d. Rebuild image + boot — now with the agent
```bash
cd ~/jai-os/vm && sudo ./make-disk.sh
cd ~/jai-os && sudo chroot build/rootfs passwd jai   # if image was rebuilt
cd ~/jai-os/vm && ./boot-vm.sh
```
Inside the VM this time:
```bash
systemctl status jai-agent   # → active (running)
cat /dev/jai                 # → JAI is awake. Kernel link established.
```

## STEP 5e — First conversation (JAI speaks from inside its own OS)

### 5e-0. NETWORK FIX (2026-10-27) — required before any conversation works
The first conversation attempt died with `Error: getaddrinfo EAI_AGAIN
opencode.ai`. Root cause was never DNS-on-its-own: **debootstrap minbase
ships no network stack at all** — no `iproute2`, no DHCP client, no
`systemd-resolved`, nothing enables `systemd-networkd`. The VM booted with
an IP-less NIC and no resolver. `resolvectl`/`systemd-resolved` don't exist
in this appliance BY DESIGN (minbase) — fixes that assume the standard
Ubuntu resolver stack are no-ops here.

Fix is now baked in (`20-jai.network` overlay + phase2 5-net + setup-rootfs):
`systemd-networkd` (already inside the `systemd` package) does DHCP from
QEMU's 10.0.2.2, and `/etc/resolv.conf` points at public resolvers straight
through slirp NAT (the 10.0.2.3 forwarder is bypassed entirely).

Re-bake (VM off):
```bash
cd ~/jai-os && git pull
sudo ./scripts/phase2.sh        # 5-net section does the work
cd vm && sudo ./make-disk.sh && ./boot-vm.sh
# verify in VM:  getent hosts opencode.ai   &&   curl -sI https://opencode.ai
```
Live-fix the RUNNING VM (no re-bake needed) — networkd is already on disk:
```bash
sudo mkdir -p /etc/systemd/network
sudo tee /etc/systemd/network/20-jai.network >/dev/null <<'EOF'
[Match]
Name=en* eth*

[Network]
DHCP=yes

[DHCPv4]
UseDNS=no
EOF
printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' | sudo tee /etc/resolv.conf
sudo systemctl start systemd-networkd
getent hosts opencode.ai         # → an address means you are online
```
NOTE: `ping` lies under slirp (ICMP needs host ping_group_range) — probe
with `getent`/`curl`, never ping. If your network blocks 1.1.1.1/8.8.8.8,
re-bake with `sudo env JAI_VM_DNS="<your resolvers>" ./scripts/phase2.sh`.

### 5e-1. Credentials — v2 stores auth in a DATABASE, not auth.json

**v2 AUTH MODEL (binary-verified 2026-10-27, strings of the 2.2.0 build):**
`opencode auth login` → "OpenCode Console" → device flow (URL + code) →
the credential becomes a ROW in the `credential` table of
`~/.local/share/opencode/opencode-next.db` (SQLite). The old
`auth.json` is only a legacy import source (DB migration
`20260805200742_import_legacy_credentials`). **"Connected ✓" followed by
`ls: auth.json: No such file or directory` is EXPECTED in v2** — the host
login DID succeed; there is just no file with that name anymore.

CANONICAL FLOW — log in from INSIDE the VM (no file copying at all).
The device flow needs no local callback (proven on the host: URL + code),
so it works over the serial console or SSH:
```bash
# IN THE VM (serial or ssh -p 2222 jai@localhost):
opencode auth login
#   → Select integration: OpenCode Console
#   → it prints a NEW code + URL — open the URL in the KALI HOST browser,
#     approve, done. (Codes are single-use: never reuse the one shown earlier.)
```
If the VM was rebooted since the network fix, networkd needs enabling once
(`start` alone does not survive reboots) — and minbase lacks the localhost
hosts entry (real gap, fixed in setup-rootfs.sh for future bakes):
```bash
sudo systemctl enable --now systemd-networkd   # persistent + immediate
printf '127.0.0.1 localhost\n' | sudo tee -a /etc/hosts
getent hosts opencode.ai                        # sanity: resolves now
```
Then the daemon home (Phase 4 gateway — the service runs with
HOME=/var/lib/jai and will read the credential DB from there):
```bash
sudo mkdir -p /var/lib/jai/.local/share/opencode
sudo cp ~/.local/share/opencode/opencode-next.db /var/lib/jai/.local/share/opencode/
sudo chown -R 1000:1000 /var/lib/jai/.local
```
Re-bake note: the DB lives on the VM disk image — it survives VM reboots
but a full rootfs rebuild (make-disk from a fresh build/rootfs) wipes it:
either repeat the in-VM login (~1 min), or after a HOST `opencode auth
login`, copy the HOST's `~/.local/share/opencode/opencode-next.db` into
`build/rootfs/{home/jai,var/lib/jai}/.local/share/opencode/` (chown
1000:1000, chmod 600) before make-disk. NEVER commit credential files.

Alternative (durable, recommended long-term): a real provider key via
`opencode auth login` (Anthropic/OpenAI/Google/…) — the free tier WILL
keep hitting limits; the error's "Connect a provider" is the sustainable
route. Host-legacy note: if you prefer file-based bootstrap anyway, the
host's legacy `auth.json` (v1 layout) is imported by the v2 DB migration
on first run — but the in-VM device login above is simpler.

VERIFIED LIVE (2026-10-27): with networking fixed, the conversation error
became "Free usage limit reached. Connect a provider or subscribe to
OpenCode Go" — a response FROM opencode.ai. The anonymous free pool is
tiny and shared per IP; a logged-in (free) Zen account gets a fresh
per-account quota. Gotcha: the ssh/scp helper block from an earlier
revision belongs in the KALI terminal, not the VM serial console (pasting
it in-VM just produces harmless "Could not resolve hostname localhost"
noise — which is ALSO how we found the missing /etc/hosts localhost line).

Inside the VM (serial console or `ssh -p 2222 jai@localhost`):
```bash
opencode models                     # what your credentials unlock
OPENCODE_CONFIG=/etc/jai/opencode.json opencode run --standalone \
  "JAI, introduce yourself in one sentence"
```
- `--standalone` — run a private server instead of attaching to the pinned
  4096 daemon (that endpoint belongs to the Phase 4 gateway)
- `OPENCODE_CONFIG=/etc/jai/opencode.json` — identity + permissions + MCP
- if it complains about a model: add `-m provider/model`
  (format shown by `opencode models`)

## STEP 6 — Phase 2b: mkosi bootable image (optional now, needed for hardware)
Refine `image/mkosi.conf` and run `./build.sh image`. Defer until the
direct-kernel-boot VM is stable — mkosi is packaging polish, not new function.

## STEP 7 — Phase 3: kernel event system — FIRST CUT DONE (2026-10-27)
`jai_dev.c` v0.2.0 ships a 4 KiB event ring buffer: write() pushes an
event into the ring AND the kernel log; read() drains pending events and
falls back to a status line when empty (so `cat /dev/jai` always means
something — the MCP jai_kernel_status tool relies on this). The module is
rebuilt automatically by `phase2.sh` (step 5c) on every re-bake. Test in
the VM:
```bash
echo "gate opened" > /dev/jai    # event → ring + kernel log
cat /dev/jai                     # → drains: gate opened
cat /dev/jai                     # → JAI is awake... + events/opens stats
journalctl -k | grep 'jai: event'    # durable kernel-log mirror
```
Remaining: runtime kernel→agent PUSH (poll / netlink / uevent) so the OS
can wake the agent — today the agent can only pull.

## STEP 7b — Phase 3b: the C hands (jai-mcp) — IN THE VM, one command

`agent/mcp/jai-mcp.c` supersedes `jai-system.py` (same tool names, plus
`disk_usage`, `service_status`, and the double-locked `service_restart`).
Live-install it into the RUNNING VM straight from the repo (the VM's
network works — use it; no more giant copy-paste blocks):

```bash
# IN THE VM (serial or ssh -p 2222 jai@localhost):
curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-jai-mcp.sh | sudo bash
```
(The script: installs gcc+libjson-c-dev, fetches the C source, compiles to
`/usr/bin/jai-mcp`, installs the sudoers allowlist + heartbeat test rig,
adds jai to adm/systemd-journal, and refreshes `/etc/jai/opencode.json` +
`/etc/jai/IDENTITY.md` with a `.bak` backup. Read it before piping it to
bash — it is short.)

Then:
```bash
exit                                          # relogin: journal group needs a new session
# protocol smoke test (no AI involved):
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"jai_sysinfo","arguments":{}}}' \
  | /usr/bin/jai-mcp
# the brain picks up the hands:
OPENCODE_CONFIG=/etc/jai/opencode.json opencode run --standalone \
  "Use your jai-mcp tools: report jai_sysinfo, disk_usage on /, and whether jai-heartbeat.service is healthy."
OPENCODE_CONFIG=/etc/jai/opencode.json opencode run --standalone \
  "restart jai-heartbeat.service, then confirm its state"
OPENCODE_CONFIG=/etc/jai/opencode.json opencode run --standalone \
  "write the event 'first touch' to /dev/jai with jai_kernel_event, then read the kernel log with jai_dmesg and show me both"
```
PASS = JAI cites measured numbers, the restart succeeds via the sudo
allowlist, and the kernel log shows `jai: event: first touch`. Expect the
occasional 503 from the free pool — retry, or `-m openrouter/google/gemma-4-31b-it:free`.

For future image bakes, `phase2.sh` step 5m now builds jai-mcp inside the
chroot and deploys the sudoers + heartbeat units automatically.

### 5e-3. Brain model pinned to Zen (2026-11-02) — what we learned the hard way
- `model` is now `opencode/mimo-v2.6-flash-free` (OpenCode Zen free tier),
  live-VM verified with MCP tools + one-shot reports. `autoupdate: false`
  is set — the appliance pins its brain; upgrades go through
  `scripts/vm-upgrade-brain.sh` (db backup → upgrade → smoke test), never
  unattended self-updates.
- v2.2.0 stores credentials in `~/.local/share/opencode/opencode*.db`
  (SQLite). There is NO auth.json — "Connected ✓" plus a missing
  auth.json file is EXPECTED, not a bug.
- **Service-account API keys carry NO free quota**: login succeeds, the
  first call returns "Free usage limit reached". Log in with the personal
  console account (device flow) instead — that quota works.
- The `jai` wrapper is now shipped by the repo
  (`rootfs/overlay/usr/local/bin/jai`): one-shot brain call with
  `OPENCODE_CONFIG` baked in and a per-call override:
  `JAI_MODEL=<provider/model> jai "..."`.
- Refresh the whole JAI layer from repo main anytime (temp-download →
  compile-before-install → .bak everything):
```bash
curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-update.sh | sudo bash
```

## STEP 7c — Phase 3c: the reflex arc (jai-eventd + jai_events)

Until now the agent could only PULL from the kernel when a human asked.
`jai-eventd` closes that gap: it drains /dev/jai continuously into
`/var/lib/jai/events.jsonl` (with ring-overflow GAP markers, so the brain
knows when telemetry has holes), and the new `jai_events` MCP tool serves
the tail. Design + format: `agent/eventd/README.md`. Why a drain-poller
and not a blocking reader: the device NEVER blocks — an empty ring
returns the status frame — see DECISIONS.md 2026-11-02.

Install into the RUNNING VM, one command (like 7b):
```bash
curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-eventd.sh | sudo bash
```
(The script compiles jai-eventd in the VM, installs the unit + logrotate,
fixes the heartbeat unit — the old `date "+%F %T"` in ExecStart could
never work, systemd eats % — replaces it with `jai-heartbeat-beat`, which
also records the kernel-event delta per beat.)

Acceptance (run in order):
```bash
systemctl status jai-eventd                        # → active (running)
echo "reflex test" > /dev/jai && sleep 1 && tail -1 /var/lib/jai/events.jsonl
#   → {"ts":"...","seq":N,"text":"reflex test"}
curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-doctor.sh | bash
#   → 'appliance healthy' (kernel link round-trip, hands, reflex arc, timers, brain)
jai "Use jai_events to report the last 3 kernel events."
#   PASS = the brain cites the measured records via the hands, no shell fallback
```
Also verify reboot persistence: `sudo systemctl reboot`, then
`systemctl is-enabled jai-eventd && tail -2 /var/lib/jai/heartbeat.log`
(the beat line now carries `events_delta=`).

Future bakes: `phase2.sh` step 5n builds jai-eventd inside the chroot and
enables the unit automatically; `provision.sh` runs the whole pipeline
(rootfs → setup → phase2 → disk) in one command.

## STEP 8 — Phase 4: voice
`agent/gateway/` — openWakeWord → whisper.cpp → Piper, wired to
`http://127.0.0.1:4096` (jai-agent). Bake Tailscale into the image.

## STEP 9 — Phase 5: real hardware
Flash `image/jarvisos.img` (or write a GRUB-less direct-boot via the
host's bootloader), boot, and JAI says hello on real metal.

---
**Golden rule:** everything that makes it "JAI OS" lives in
`rootfs/overlay/`, `kernel/`, `agent/`, and `scripts/`. The Ubuntu base is
disposable — `rm -rf build/` and rebuild anytime.

### 5e-2. Free-limit even AFTER login (2026-10-27) → connect a provider key
Verified: in-VM `opencode auth login` (device flow) WORKED — proof: the
model list expanded from 9 `*-free` models to the full paid catalog
(claude/gpt/gemini/glm...). But `opencode run` still returns "Free usage
limit reached": the `opencode/*-free` pool is tiny/oversubscribed and an
account barely changes it. The durable fix is the error's own advice —
"Connect a provider":
1. Get a key: Google AI Studio (aistudio.google.com → Get API key, has a
   real free tier), or OpenRouter (one key, `:free` models), or any key
   you already own (Anthropic/OpenAI/...).
2. In the VM: `opencode auth login` → select that provider → paste key.
3. `opencode models` now lists the new provider's models.
4. Run: `OPENCODE_CONFIG=/etc/jai/opencode.json opencode run --standalone \
   -m <provider>/<model> "JAI, introduce yourself in one sentence"`
   (pass `-m` explicitly — the default agent may still pick a `*-free`
   model). Worth one cheap try first: `-m opencode/glm-5.3-flash-free`.
CLI gotcha: `opencode model` (no s) is parsed as the `directory` argument
→ "ENOENT ... chdir '/home/jai' -> 'model'". The subcommand is `models`.
