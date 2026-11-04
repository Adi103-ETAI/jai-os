# JAI

**JAI OS is an AI-assisted desktop operating system** built on top of Ubuntu —
like Windows, Ubuntu or macOS, but with an AI woven into the OS itself: it
watches the machine, explains things in plain words, fixes what is safe to
fix, and helps control the system — while the user can always just type,
click, and use the computer normally.

Two laws (full text: [docs/VISION.md](docs/VISION.md)):
1. **Boundless to hardware** — runs on anything from a 2 GB headless box to
   a gaming rig: it MEASURES the machine and adapts, degrades gracefully,
   never REQUIRES power. Our workshop laptop is the test bench, not the ceiling.
2. **The user is the boss** — the AI assists and proposes; it never takes the
   keyboard away; every dangerous action passes a permission gate.

This repo holds the build system, the JAI layer (kernel module, reflex
arc, MCP hands, brain config), and the QEMU bench that proves everything
before it touches real metal.

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
| [docs/VISION.md](docs/VISION.md) | **the north star: what we are building and the two laws** |
| [docs/CODE-MAP.md](docs/CODE-MAP.md) | what code is written, what each piece does, how it builds |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | layer map, runtime + build flows, phase table |
| [docs/ARCHITECTURE-V2.md](docs/ARCHITECTURE-V2.md) | the active-system design (warm brain, gate, tiers) |
| [docs/STATUS.md](docs/STATUS.md) | exactly what's done / partial / not started |
| [docs/NEXT-STEPS.md](docs/NEXT-STEPS.md) | **what to do next — exact commands, in order** |
| [docs/DECISIONS.md](docs/DECISIONS.md) | every design decision + why |
| [docs/GPU-ACCEL.md](docs/GPU-ACCEL.md) | how tier-1 runs 100% on the Vega iGPU (Ollama + Vulkan) |
| [docs/CAPABILITIES-SPEC.md](docs/CAPABILITIES-SPEC.md) | roadmap 2b — the boot-time hardware probe: schema, ladder, fallbacks |

## Hardware context

Workshop rig (our dev/test machine, NOT a product requirement): AMD Athlon
Silver 3050U (2 cores, 3.2 GHz), 9.7 GB RAM, no dedicated GPU, headless.
Everything here is integration work — no kernel compiles — so this modest
box is sufficient for building and testing. The Vega iGPU serves tier-1 at
100% GPU via Vulkan (docs/GPU-ACCEL.md). The PRODUCT is hardware-boundless
(docs/VISION.md): it measures whatever machine it lands on and adapts.

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
