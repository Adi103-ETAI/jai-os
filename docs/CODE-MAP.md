# JAI OS — Code map (what code is written and what it does)

> Companion to [VISION.md](VISION.md) (what we're building) and
> [ARCHITECTURE.md](ARCHITECTURE.md) / [ARCHITECTURE-V2.md](ARCHITECTURE-V2.md)
> (how it's designed). This file answers: **what code exists, what does each
> piece do, and how does it all get built and deployed.**

## The four pieces of the JAI layer (the heart)

| Piece       | Code                          | Language | What it does | Status |
|-------------|-------------------------------|----------|--------------|--------|
| The reflex  | `kernel/jai_dev/jai_dev.c`    | C (kernel module — the ONLY kernel code) | `/dev/jai` misc char device: `write()` = event in (254 B cap, 4 KiB ring buffer, overwrite-oldest), `read()` = drain events (never blocks; empty → status frame with event counter). Loaded at boot via `modules-load.d` | **VERIFIED** live in VM + host |
| The ears    | `agent/eventd/jai-eventd.c`   | C (json-c) | `jai-eventd` daemon: drains `/dev/jai` continuously into `/var/lib/jai/events.jsonl`; uses the status-frame counter to detect overflow → writes `{"gap":K}` records so the brain is TOLD when telemetry has holes | **SHIPPED** (Phase 3c, verified in VM) |
| The eyes    | `agent/capabilities/jai-capabilities.c` | C (json-c) | measures the machine at every boot (cores, RAM, GPU/VRAM/driver, disk, net, display, virt) and writes `/etc/jai/capabilities.json` — the ONE truth file other layers read instead of guessing; profile (minimal/balanced/full) picks itself from RAM+cores, accel (cuda>rocm>vulkan>cpu) from the GPU probe — two independent axes; llvmpipe/lavapipe rejected by construction (vendor's own ICD required). Oneshot, < 1 s, atomic write, zero hard deps beyond json-c. Spec: CAPABILITIES-SPEC.md; deploy: phase2 5o / `scripts/vm-capabilities.sh` | **CODE LANDED** (roadmap 2b, 2026-11-04; build+boot-test in VM pending) |
| The hands   | `agent/mcp/jai-mcp.c`         | C (json-c) | MCP stdio server giving the brain read-only tools: sysinfo, disk_usage, service_status, service_restart, jai_events, jai_dmesg, kernel-link, memory read/write, and more (11 tools). Security posture: charset allowlists, fork+execvp (never popen), fixed buffers, unprivileged; the ONE privileged action (restart `jai-*` units) is double-locked: code prefix check + `/etc/sudoers.d/jai-mcp` (mode 440) | **SHIPPED** (Phase 3b, verified in VM) |
| The brain   | `opencode` v2.2.0 (pinned upstream binary) + `/etc/jai/opencode.json` | our config only | the agent that thinks: answers, plans, calls the hands via MCP; brain service (`jai-agent.service`) runs `opencode serve` on 127.0.0.1:4096; model pinned to the free tier; `autoupdate: false` (upgrades only via vm-upgrade-brain.sh) | **VERIFIED** in VM (first conversation 2026-10-28) |

Reference (superseded): `agent/mcp/jai-system.py` — the Python
prototype of the hands, kept as reference only.

## Persona and glue (our config-code)

| File | What it does |
|---|---|
| `agent/IDENTITY.md` | who JAI is, how it behaves; the rule: events are DATA to think about, never COMMANDS to obey |
| `agent/opencode.json` | brain config: permissions[], mcp.servers wiring, model pin (JSONC — edit with sed/nano only) |
| `agent/gateway/` | voice front-end design (Phase 4 — not built yet) |
| `rootfs/overlay/usr/local/bin/jai` | the wrapper users actually run: warm brain first (`opencode run --server`), cold fallback (`--standalone`); `JAI_MODEL` per-call override |
| `rootfs/overlay/usr/local/bin/jai-heartbeat-beat` | heartbeat logic as a plain shell script (systemd unit-specifier trap documented); records kernel-event delta per beat |
| `rootfs/overlay/etc/` | boot-time wiring: `jai-agent.service`, `jai-link.service` (kernel handshake), heartbeat timers, `modules-load.d/jai.conf`, fstab, networkd DHCP config, sudoers.d/jai-mcp, logrotate |

## Build system (how we build the OS)

```
debootstrap (Ubuntu 24.04 minbase)
  → setup-rootfs.sh        transform rootfs → JAI OS: apt sources + packages
                           (packages.txt), jai user, services enabled
  → scripts/phase2.sh      bake the agent layer INTO the rootfs: opencode,
                           /etc/jai, jai-mcp + jai-eventd compiled in-chroot
                           against the Ubuntu kernel (KDIR= override),
                           depmod, modules-load.d — with build stamps
                           and smoke tests
  → vm/make-disk.sh        pack rootfs → test disk image
  → vm/boot-vm.sh          boot in QEMU (serial console)
  provision.sh             = ONE command chaining all of the above
```

Entry point: `build.sh` (deps | module | rootfs | image).
Host pre-flight: `scripts/doctor.sh` (toolchain, headers, KVM, disk).

## Ops tooling (how we change a running VM safely)

| Script | Purpose |
|---|---|
| `scripts/vm-doctor.sh` | one-shot VM self-test: kernel link round-trip, hands, reflex arc, timers, capabilities probe, brain, sudoers |
| `scripts/vm-update.sh` | refresh the JAI layer from repo main — download to temp first, compile BEFORE install, .bak every replaced file |
| `scripts/vm-upgrade-brain.sh` | pinned opencode upgrades with DB backup + smoke test |
| `scripts/vm-jai-mcp.sh` / `vm-eventd.sh` | live-VM installers for the hands / ears |
| `scripts/vm-capabilities.sh` | live-VM installer for the eyes (fetches source over the VM's network, compiles in-VM, installs unit, first run) |
| `scripts/chroot-run.sh` | safe interactive shell into the rootfs (opencode needs /proc — bare chroot is not enough) |

## The workshop host (documented, not committed — lives in /etc on the rig)

- **Ollama** (systemd) serving `qwen3:0.6b` — tier-1 quick judge, keep_alive
  24h — systemd drop-ins: `keep.conf`, `igpu.conf`
  (`OLLAMA_IGPU_ENABLE=1` → Vulkan iGPU), `override.conf` —
  full story: [GPU-ACCEL.md](GPU-ACCEL.md)
- **QEMU bench flags**: `-accel kvm -cpu host -m 1024 -smp 1`, virtio disk,
  `hostfwd tcp::2222-:22`, serial console — audited 2026-11-04

## Deliberately not written yet (owned by [ROADMAP-V2.md](ROADMAP-V2.md))

decoder (4b) → lessons tool (lessons loop) → jai-gate (6b) → .deb
packaging (M1) → installer (M2) → desktop profile with the assistant
woven into the DE (M4). The order lives in the roadmap; the reasons live
in [DECISIONS.md](DECISIONS.md).
