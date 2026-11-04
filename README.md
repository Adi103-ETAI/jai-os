# JAI

**JAI** ("Jay") is a persistent, headless OS-level assistant built **on top of**
Ubuntu — not a kernel fork. Borrow the base, own the layers above.

```
┌───────────────────────────────────────────────┐
│ JAI LAYER (ours — all custom code)            │
│  jai-gateway   : voice/IO front-end (Phase 4) │
│  jai-agent     : opencode serve, the brain    │
│  jai-mcp       : custom MCP tool servers      │
│  kernel/jai_dev: /dev/jai kernel interface    │
├───────────────────────────────────────────────┤
│ BASE (borrowed)                               │
│  Ubuntu 24.04 minimal userland (debootstrap)  │
├───────────────────────────────────────────────┤
│ KERNEL (borrowed)                             │
│  Stock Ubuntu linux-image-generic             │
│  + our out-of-tree modules only               │
└───────────────────────────────────────────────┘
```

## Runtime flow

```
voice/CLI/API ──► jai-gateway (systemd)
                      │ wake word, STT, intent
                      ▼
              jai-agent = opencode serve (HTTP API)
                      │
        ┌─────────────┼──────────────┐
        ▼             ▼              ▼
   jai-mcp        kernel link     NAS / Tailscale
   tool servers   /dev/jai        (memory, devices)
```

## Build flow

```
debootstrap minimal rootfs
  → copy rootfs/overlay/ in          (our systemd services + files)
  → setup-rootfs.sh                  (apt packages, jai user, enable services)
  → vm/make-disk.sh                  (pack rootfs into a test disk)
  → vm/boot-vm.sh                    (boot in QEMU, serial console)
  → image/mkosi.conf                 (Phase 2b: real bootable image)
  → real hardware                    (Phase 5)
```

## Docs

| Document | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | layer map, runtime + build flows, phase table |
| [docs/STATUS.md](docs/STATUS.md) | exactly what's done / partial / not started |
| [docs/NEXT-STEPS.md](docs/NEXT-STEPS.md) | **what to do next — exact commands, in order** |
| [docs/DECISIONS.md](docs/DECISIONS.md) | every design decision + why |
| [docs/GPU-ACCEL.md](docs/GPU-ACCEL.md) | how tier-1 runs 100% on the Vega iGPU (Ollama + Vulkan) |

## Hardware context

Build host: AMD Athlon Silver 3050U (2 cores, 3.2 GHz), 9.7 GB RAM, headless.
Everything here is integration work — no kernel compiles — so this modest
box is sufficient. Test VMs are capped at 1 GB RAM / 1 vCPU by design.
The chip's Vega iGPU now serves tier-1 at 100% GPU via Vulkan
(docs/GPU-ACCEL.md) — headless turns out to be ideal for that.

## Repo layout

```
kernel/jai_dev/      # out-of-tree kernel module: /dev/jai char device + event ring
rootfs/overlay/      # our files layered onto the Ubuntu base (services, sudoers,
                     #   logrotate, /usr/local/bin scripts: jai wrapper, beat)
rootfs/packages.txt  # extra apt packages JAI adds (systemd/sysv + kmod pinned)
agent/               # opencode config, JAI persona, jai wrapper, MCP + eventd + gateway
  agent/mcp/         #   jai-mcp.c — the hands (C MCP server, 11 tools)
  agent/eventd/      #   jai-eventd.c — the reflex arc (kernel events -> JSONL)
scripts/             # doctor.sh (host pre-flight), phase2.sh (bake-in),
                     #   vm-jai-mcp.sh / vm-eventd.sh (live-VM installers),
                     #   vm-doctor.sh (VM self-test), vm-update.sh (safe refresh),
                     #   vm-upgrade-brain.sh (pinned brain upgrades)
image/mkosi.conf     # bootable image config (Phase 2b)
vm/                  # test VM scripts + docs
docs/                # architecture, status, next steps, decisions
build.sh             # entry point: deps | module | rootfs | image
provision.sh         # ONE-COMMAND rebuild: doctor -> rootfs -> setup -> phase2 -> disk
setup-rootfs.sh      # rootfs → JAI OS transformation (needs sudo)
```
