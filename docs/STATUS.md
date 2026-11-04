# JAI — Status

Last updated: 2026-11-04 (Tier-1 LIVE on Vega iGPU via Ollama+Vulkan; llama.cpp retired; KVM verified in QEMU cmdline)

Legend: ✅ done · 🟡 partial/designed but not baked in · ⬜ not started

## Phase 0 — Playground + first kernel code ✅ COMPLETE
- ✅ Toolchain installed (gcc, make, clang, ccache, kernel headers)
- ✅ `kernel/jai_dev/` — real kernel module, misc device `/dev/jai`
  - read → "JAI is awake. Kernel link established."
  - write → userland events land in kernel log (`jai: event: ...`)
- ✅ Compiled (`jai_dev.ko`) against Kali 7.1.5 kernel, **loaded and verified live**
  (kernel log evidence captured; see DECISIONS.md)
- ✅ Git history started; pushed to GitHub

## Phase 1 — Rootfs boots in test VM ✅ COMPLETE (first boot 2026-10-27)
- ✅ `debootstrap` Ubuntu 24.04 (noble) minimal rootfs — 1.5 GB at `build/rootfs/`
- ✅ `setup-rootfs.sh` transformed it into JAI OS:
  - apt sources (noble + updates + security) written
  - package set installed — **including Ubuntu kernel `linux-image-6.8.0-147-generic`**
  - hostname `jai`, `jai` user created (uid 1000, in sudo group)
  - `/var/lib/jai-memory` created
  - `jai-agent.service` **enabled via systemctl**
- ✅ `vm/make-disk.sh` + `vm/boot-vm.sh` written (fixed live-mount race)
- ✅ **FIRST VM BOOT 2026-10-27**: serial console OK, `jai` login OK, sshd
  socket up, `jai-agent.service` started (crash-looped — see Phase 2 note)
- ✅ opencode SIGILL under qemu64 CPU — FIXED and confirmed: the re-baked
  VM runs `opencode v2.2.0` clean (baseline build + `-cpu max` under TCG)

## Phase 2 — Agent layer baked in ✅ VERIFIED IN VM (boot 2026-10-27 15:39 UTC)
- 🟡 **Agent layer status:**
  - ✅ `jai-agent.service` written + enabled (runs `opencode serve` as daemon)
  - ✅ `agent/IDENTITY.md` persona + `agent/opencode.json` permissions written
  - ✅ `scripts/phase2.sh` — one-command bake-in (opencode, /etc/jai, in-chroot
    module build with KDIR override, depmod, modules-load.d)
  - ✅ OpenCode **v2 (2.2.0) pinned** — installed from npm tarball directly
    (v1 installer refuses 2.x; v2 installer has no PREFIX); config rewritten
    in v2 schema (`permissions[]`, `mcp.servers.*`), verified loading on
    the real 2.2.0 binary
  - ✅ `scripts/doctor.sh` — build host pre-flight check
  - ✅ overlay additions: `etc/modules-load.d/jai.conf`, `etc/fstab`,
    `etc/systemd/system/jai-link.service` (boot kernel handshake)
  - ✅ `packages.txt` hardened: `systemd` + `systemd-sysv` explicit (minbase
    does not guarantee PID 1), `kmod` for depmod/modprobe
  - ✅ `opencode` 2.2.0 (baseline) IS in the rootfs — VM shows
    `opencode v2.2.0`, `jai-agent.service` active (running, opencode serve)
- 🟡 `jai_dev` module: compiled for **Kali** kernel — VM runs **Ubuntu 6.8**,
  so the module must be rebuilt *inside the chroot* against Ubuntu headers
  (`phase2.sh` handles this; inside a chroot `uname -r` is the HOST kernel,
  so the Makefile gets an explicit `KDIR=`)
- ✅ `modules-load.d/jai.conf` so JAI OS loads its module at boot (overlay + phase2.sh)
- ✅ re-bake + verification DONE (2026-10-27 15:39 UTC VM boot): jai-agent
  active (PID 247, opencode serve), jai-link handshake OK ("JAI is awake.
  Kernel link established."), `cat /dev/jai` OK.
  NOTE: interactive `dmesg` → EPERM is EXPECTED (Ubuntu ships
  kernel.dmesg_restrict=1). Use `sudo dmesg` or `journalctl -k` (jai is in
  sudo + systemd-journal/adm; the agent's jai_dmesg MCP tool uses
  `journalctl -k`, so the agent is unaffected).
- ✅ agent server pinned (phase2.sh 5a-pin): v2 `serve` binds a RANDOM port
  when unconfigured (VM picked 49374) and its API always demands HTTP Basic
  auth → baked `/var/lib/jai/.config/opencode/service.json` via
  `opencode service set port 4096` + `service set password jai`
  (see DECISIONS.md + agent/gateway/README.md)
- ✅ VM NETWORKING (2026-10-27): minbase rootfs had NO network stack —
  no iproute2, no DHCP client, no systemd-resolved, networkd never enabled
  → first conversation died with `getaddrinfo EAI_AGAIN`. Baked:
  `20-jai.network` overlay (networkd DHCP from 10.0.2.2, UseDNS=no),
  networkd enablement (phase2 5-net + setup-rootfs), static appliance
  `/etc/resolv.conf` (public resolvers through slirp NAT; `JAI_VM_DNS`
  override), `iproute2` in packages.txt. See DECISIONS.md.
- ✅ VM NETWORKING **VERIFIED LIVE** (2026-10-27): after the live networkd
  fix in the running VM, the error progressed from `getaddrinfo EAI_AGAIN`
  (no network) to "Free usage limit reached" — a response FROM opencode.ai.
  Full chain proven: DHCP → slirp NAT → DNS → TLS → API. Gotcha logged:
  `systemctl start systemd-networkd` returns before DHCP has a lease — the
  first lookups seconds later still fail; the request a minute later works.
- ⬜ give the agent credentials → first conversation inside JAI OS — the
  v2 auth model changed (binary-verified 2026-10-27): `opencode auth login`
  (device flow) writes the credential into the `credential` table of
  `~/.local/share/opencode/opencode-next.db` — **auth.json is legacy-only
  in v2** ("Connected ✓" + no auth.json file is EXPECTED). Canonical: log
  in from INSIDE the VM (device flow needs no local callback; open the
  printed URL on the host browser), then copy the DB to /var/lib/jai for
  the Phase 4 gateway. See NEXT-STEPS 5e-1.
- ✅ FIRST CONVERSATION (2026-10-28, live VM): after in-VM provider logins
  (OpenRouter + Google AI Studio), `opencode run --standalone` answered.
  Two ops lessons baked into `agent/opencode.json`: v2 config wants
  `mcp.servers.<name>.disabled` (NOT v1's `mcp.<name>.enabled`), and
  OpenRouter pre-authorizes the model's full max output against the
  balance — free models + `limit.output: 4096` sidesteps the credit check
  entirely ("requested 128000, could only afford 7469" incident).
- ⬜ mkosi bootable image (optional — direct-kernel boot works for testing)

## Phase 3 — Kernel event system 🟡 RING BUFFER DONE (v0.2.0 module)
- ✅ **agent↔kernel/OS bridge:** `agent/mcp/jai-system.py` — zero-dependency
  MCP stdio server (raw JSON-RPC 2.0) exposing /dev/jai read/write, dmesg,
  jai-* service health, sysinfo, and /var/lib/jai-memory to the agent;
  wired into `opencode.json` → mcp; deployed by `phase2.sh` 5b+
- ✅ **Phase 3b — the C hands:** `agent/mcp/jai-mcp.c` — the bridge rewritten
  in C against libjson-c, supersedes jai-system.py (same tool names, kept
  as reference) and ADDS `disk_usage`, `service_status`, `service_restart`.
  Security posture: charset allowlists on every argument, fork+execvp only
  (never popen), fixed-size buffers, json-c for all JSON, unprivileged
  user. The ONE privileged action (restart `jai-*` units) is double-locked:
  prefix check in code + `/etc/sudoers.d/jai-mcp` (mode 440). Built inside
  the chroot (`phase2.sh` 5m) against the rootfs glibc; live-VM path:
  `scripts/vm-jai-mcp.sh` (fetches from the repo over the VM's own network
  — no more giant pastes). Known limit: under jai-agent.service,
  NoNewPrivileges blocks sudo by design (gateway phase gets a helper).
  See agent/mcp/README.md.
- ✅ `jai-link.service` — boot-time kernel handshake (cat /dev/jai + boot
  event into kernel log); jai user added to adm/systemd-journal for dmesg
- ✅ `/dev/jai` event ring buffer (`jai_dev.c` v0.2.0): 4 KiB static ring —
  write() pushes an event into the ring AND the kernel log (journalctl -k
  mirror kept); read() DRAINS pending events, empty → status line via
  ppos (so `cat /dev/jai` always terminates and stays meaningful);
  254 B/write cap, overwrite-oldest on overflow. See DECISIONS.md.
  VM test: `echo "gate opened" > /dev/jai && cat /dev/jai && cat /dev/jai`
- ✅ **Phase 3c — the reflex arc (2026-11-02, code shipped):** `jai-eventd`
  drains /dev/jai continuously into `/var/lib/jai/events.jsonl`. Because
  the device never blocks (empty ring → status frame), the daemon is a
  drain-poller; the status frame's kernel-side `events: N total` counter
  gives overflow detection → `{"gap":K}` records, so the brain is TOLD
  when telemetry has holes. JSONL built with json-c (house rule). New
  `jai_events` MCP tool (jai-mcp v1.1.0) serves the tail; `jai_services`
  now covers jai-eventd + jai-heartbeat.* too. Deploy: `phase2.sh` 5n
  (bake) or `scripts/vm-eventd.sh` (live VM). Design: agent/eventd/README.md.
- ✅ **Phase 3d — ops tooling (2026-11-02, shipped):** `scripts/vm-doctor.sh`
  (one-shot VM self-test: kernel link round-trip, hands, reflex arc,
  timers, brain, sudoers), `scripts/vm-update.sh` (validated whole-layer
  refresh: temp download → compile before install → .bak everything),
  `scripts/vm-upgrade-brain.sh` (pinned opencode upgrades with db backup +
  smoke test), `provision.sh` (one-command rebuild: doctor → rootfs →
  setup → phase2 → disk; `--fresh` for from-scratch). Heartbeat %-bug
  fixed by moving the logic into `jai-heartbeat-beat` (plain shell, no
  unit-specifier escaping; adds event delta per beat).
- ✅ Brain pinned (2026-11-02, live-VM verified): `opencode/mimo-v2.6-flash-free`
  (Zen free tier) via DEVICE-FLOW login; service-account keys carry NO
  free quota (auth succeeds, first call hits the limit). Credentials live
  in `~/.local/share/opencode/opencode*.db` (v2 has NO auth.json).
  `autoupdate: false` — upgrades only via vm-upgrade-brain.sh.
  `jai` wrapper now ships via overlay (JAI_MODEL per-call override).

## Phase 4 — Voice + remote access ⬜
- ⬜ jai-gateway: openWakeWord + whisper.cpp (STT) + Piper (TTS), event-driven
- ⬜ opencode HTTP API integration
- ⬜ Tailscale baked into image, remote voice sessions

## Phase 5 — First real-hardware boot ⬜
- ⬜ Boot JAI OS image on actual hardware

## v2.5 status snapshot (active-system redesign)

- WORKING: kernel reflexes, eventd JSONL, MCP tools, wrapper v1, provision scripts,
  tier-1 serving (Ollama on host, 100% GPU via Vulkan iGPU).
- MEASURED: cold thought 45.6 s; error path 10.87 s; warm service 434 MB / 0.4% CPU;
  tier-1 926 MB GPU-resident; host 87.5% idle with VM up (load 0.63); QEMU runs
  `-accel kvm -cpu host` (confirmed); VM RAM 961 MB (needs 2G).
- DECIDED: warm brain (6a), gate (6b), tier-1 = Ollama + Vulkan iGPU (D12/D13),
  lessons loop, product pivot.
- BLOCKED: 4b decoder (needs event struct + jai-mcp.c tail + phase2.sh from owner).
- IN FLIGHT: 6a build block sent.
- OPEN: token revoke (STILL pending, 2026-11-04); VM 2G bump; OLLAMA_HOST=0.0.0.0
  so the VM can reach tier-1 at 10.0.2.2:11434.
Details: ARCHITECTURE-V2.md, ROADMAP-V2.md, TIER1-MODEL.md, GPU-ACCEL.md.
